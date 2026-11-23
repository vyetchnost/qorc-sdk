/*==========================================================
 *
 *    File   : scheme_task.c
 *    Purpose: Scheme REPL task on the USB-serial console, plus
 *             hardware primitives for the Qomu board.
 *
 *=========================================================*/

#include "Fw_global_config.h"

#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "RtosTask.h"
#include "eoss3_hal_gpio.h"
#include "eoss3_hal_fpga_usbserial.h"
#include "scheme.h"

#ifndef SCHEME_HEAP_BYTES
#define SCHEME_HEAP_BYTES   (48 * 1024)     /* cell heap: 12 bytes per cell     */
#endif
#ifndef SCHEME_STACK_BYTES
#define SCHEME_STACK_BYTES  (20 * 1024)     /* task stack, from the FreeRTOS heap */
#endif
#ifndef SCHEME_RX_BYTES
#define SCHEME_RX_BYTES     (8 * 1024)      /* console input ring (pastes)      */
#endif

static uint8_t scheme_heap[SCHEME_HEAP_BYTES] __attribute__((aligned(8)));

/* ---------------------------------------------------------------- console I/O */

/*
 * The TX FIFO only drains while the host has the port open. Never block
 * forever: wait up to ~100 ms for room, then drop output until it drains
 * again, so the REPL keeps running with no terminal attached.
 */
static int tx_stalled;

static void q_putc(int c)
{
    int waited = 0;
    while (HAL_usbserial_tx_get_fifo_status() == FPGA_USBSERIAL_TX_FIFO_FULL) {
        if (tx_stalled || waited >= 100) {
            tx_stalled = 1;
            return;
        }
        vTaskDelay(1);
        waited++;
    }
    tx_stalled = 0;
    HAL_usbserial_putc((char)c);
}

/*
 * Console input. The USB-serial core drops bytes when its 512-byte RX FIFO is
 * full; it has no flow control. A pasted block arrives in a few milliseconds
 * while the REPL may be busy evaluating, so a separate higher-priority task
 * drains the FIFO into this ring every tick, whatever the interpreter is doing.
 * Ctrl-C is handled there too: it discards pending input and raises a flag.
 */
static uint8_t rx_ring[SCHEME_RX_BYTES];
static volatile uint32_t rx_head;           /* written by the pump  */
static volatile uint32_t rx_tail;           /* written by the REPL  */
static volatile int rx_break;

static void usb_rx_pump(void *arg)
{
    (void)arg;
    for (;;) {
        while (HAL_usbserial_dataavailable()) {
            uint32_t next = (rx_head + 1) % SCHEME_RX_BYTES;
            int c;
            if (next == rx_tail)
                break;                      /* ring full: leave the rest in the FIFO */
            c = HAL_usbserial_getc() & 0xff;
            if (c == 3) {
                taskENTER_CRITICAL();
                rx_tail = rx_head;          /* ^C cancels everything still queued */
                taskEXIT_CRITICAL();
                rx_break = 1;
                continue;
            }
            rx_ring[rx_head] = (uint8_t)c;
            rx_head = next;
        }
        vTaskDelay(1);
    }
}

static int q_avail(void)
{
    return rx_head != rx_tail;
}

static int q_getc(void)
{
    for (;;) {
        if (rx_break) {                     /* ^C while the line editor waits */
            rx_break = 0;
            return 3;
        }
        if (rx_head != rx_tail) {
            int c;
            taskENTER_CRITICAL();
            c = rx_ring[rx_tail];
            rx_tail = (rx_tail + 1) % SCHEME_RX_BYTES;
            taskEXIT_CRITICAL();
            return c;
        }
        vTaskDelay(1);
    }
}

static int q_brk(void)
{
    if (!rx_break) return 0;
    rx_break = 0;
    return 1;
}

/* ---------------------------------------------------------------- primitives */

static uint32_t addr_arg(scm_obj *args, uint32_t align)
{
    scm_obj o = scm_arg(args);
    uint32_t a = (uint32_t)scm_to_int(o);
    if (a & (align - 1)) scm_error("unaligned address:", o);
    return a;
}

/* (peek addr) -> 32-bit value at addr */
static scm_obj p_peek(scm_obj args)
{
    uint32_t a = addr_arg(&args, 4);
    return scm_from_int(*(volatile uint32_t *)a);
}

/* (poke addr value) -> writes 32 bits */
static scm_obj p_poke(scm_obj args)
{
    uint32_t a = addr_arg(&args, 4);
    *(volatile uint32_t *)a = (uint32_t)scm_to_int(scm_arg(&args));
    return SCM_UNSPEC;
}

static scm_obj p_peek8(scm_obj args)
{
    uint32_t a = addr_arg(&args, 1);
    return scm_from_int(*(volatile uint8_t *)a);
}

static scm_obj p_poke8(scm_obj args)
{
    uint32_t a = addr_arg(&args, 1);
    *(volatile uint8_t *)a = (uint8_t)scm_to_int(scm_arg(&args));
    return SCM_UNSPEC;
}

/* (sleep ms) - yields to other tasks; Ctrl-C still interrupts */
static scm_obj p_sleep(scm_obj args)
{
    int64_t ms = scm_to_int(scm_arg(&args));
    while (ms > 0) {
        int64_t chunk = ms > 20 ? 20 : ms;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)chunk) ? pdMS_TO_TICKS((uint32_t)chunk) : 1);
        ms -= chunk;
        scm_check_interrupt();
    }
    return SCM_UNSPEC;
}

/* (ms) - milliseconds since the scheduler started */
static scm_obj p_ms(scm_obj args)
{
    (void)args;
    return scm_from_int((int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* (gpio-write n bool-or-int) */
static scm_obj p_gpio_write(scm_obj args)
{
    int64_t n = scm_to_int(scm_arg(&args));
    scm_obj v = scm_arg(&args);
    int on = (v == SCM_FALSE) ? 0 : (v == SCM_TRUE) ? 1 : (scm_to_int(v) != 0);
    if (n < 0 || n > 7) scm_error("gpio-write: bad gpio number", NULL);
    HAL_GPIO_Write((uint8_t)n, (uint8_t)on);
    return SCM_UNSPEC;
}

/* (gpio-read n) -> 0 or 1 */
static scm_obj p_gpio_read(scm_obj args)
{
    int64_t n = scm_to_int(scm_arg(&args));
    uint8_t val = 0;
    if (n < 0 || n > 7) scm_error("gpio-read: bad gpio number", NULL);
    HAL_GPIO_Read((uint8_t)n, &val);
    return scm_from_int(val);
}

/*
 * (led 'red|'green|'blue on?)
 * Red (IO24) and green (IO6) share GPIO0; the pad output enables select which
 * one is driven, as in the helloworld diag commands. Blue is GPIO3.
 */
enum { LED_RED, LED_GREEN, LED_BLUE };

static void board_led(int which, int on)
{
    volatile uint32_t *pad6 = (volatile uint32_t *)0x40004c18;
    volatile uint32_t *pad24 = (volatile uint32_t *)0x40004c60;
    if (which == LED_RED) {
        *pad6 |= 0x20;              /* IO6 output off */
        *pad24 &= ~0x20u;           /* IO24 output on */
        HAL_GPIO_Write(0, (uint8_t)on);
    } else if (which == LED_GREEN) {
        *pad24 |= 0x20;
        *pad6 &= ~0x20u;
        HAL_GPIO_Write(0, (uint8_t)on);
    } else {
        HAL_GPIO_Write(3, (uint8_t)on);
    }
}

static scm_obj p_led(scm_obj args)
{
    const char *name = scm_to_cstr(scm_arg(&args));
    int on = scm_is_true(scm_arg(&args));
    if (!strcmp(name, "red")) board_led(LED_RED, on);
    else if (!strcmp(name, "green")) board_led(LED_GREEN, on);
    else if (!strcmp(name, "blue")) board_led(LED_BLUE, on);
    else scm_error("led: expected red, green or blue", NULL);
    return SCM_UNSPEC;
}

/* (heap-free) -> bytes free in the FreeRTOS heap (strings, vectors) */
static scm_obj p_heap_free(scm_obj args)
{
    (void)args;
    return scm_from_int((int64_t)xPortGetFreeHeapSize());
}

#if INCLUDE_uxTaskGetStackHighWaterMark
/* (stack-free) -> bytes of this task's stack never used so far */
static scm_obj p_stack_free(scm_obj args)
{
    (void)args;
    return scm_from_int((int64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
}
#endif

static const char board_prelude[] =
"(define (hex n) (string-append \"#x\" (number->string n 16)))\n"
"(define (blink color n ms)\n"
"  (dotimes (i n) (led color #t) (sleep ms) (led color #f) (sleep ms)))\n"
"(define (help)\n"
"  (display \"Qomu extras: (peek a) (poke a v) (peek8 a) (poke8 a v) (sleep ms) (ms)\\n\")\n"
"  (display \"  (led 'red|'green|'blue on?) (blink 'blue 5 200) (gpio-write n v) (gpio-read n)\\n\")\n"
"  (display \"  (hex n) (room) (gc) (heap-free) (stack-free)\\n\")\n"
"  (display \"Ctrl-C interrupts a running program; up-arrow recalls the last line.\\n\"))\n";

/* ---------------------------------------------------------------- task */

static void scheme_task(void *arg)
{
    static const scm_io io = { q_putc, q_getc, q_avail, 1, q_brk };
    (void)arg;

    /* Boot checkpoints (temporary): blue = in scm_init, green = waiting for
     * the first keypress, all off = REPL running. */
    board_led(LED_BLUE, 1);
    scm_init(scheme_heap, sizeof scheme_heap, &io);
    scm_set_stack_limit(SCHEME_STACK_BYTES - 3 * 1024);

    scm_defprim("peek", p_peek, 1, 1);
    scm_defprim("poke", p_poke, 2, 2);
    scm_defprim("peek8", p_peek8, 1, 1);
    scm_defprim("poke8", p_poke8, 2, 2);
    scm_defprim("sleep", p_sleep, 1, 1);
    scm_defprim("ms", p_ms, 0, 0);
    scm_defprim("gpio-write", p_gpio_write, 2, 2);
    scm_defprim("gpio-read", p_gpio_read, 1, 1);
    scm_defprim("led", p_led, 2, 2);
    scm_defprim("heap-free", p_heap_free, 0, 0);
#if INCLUDE_uxTaskGetStackHighWaterMark
    scm_defprim("stack-free", p_stack_free, 0, 0);
#endif
    scm_eval_cstr(board_prelude);
    board_led(LED_BLUE, 0);
    board_led(LED_GREEN, 1);

    /* Print the banner once someone is typing, i.e. a terminal is open. */
    while (!q_avail())
        vTaskDelay(10);
    board_led(LED_GREEN, 0);

    scm_puts("\nQomu Scheme - ");
    scm_display(scm_from_int((int64_t)scm_free_cells()));
    scm_puts(" cells free. Type (help) for board commands.\n");

    for (;;)
        scm_repl();         /* only returns on EOF, which the tty never sends */
}

void scheme_start_task(void)
{
    /* Input pump runs above the REPL so it drains the USB FIFO every tick. */
    if (xTaskCreate(usb_rx_pump, "UsbRx", 256, NULL,
                    (UBaseType_t)PRIORITY_NORMAL + 1, NULL) != pdPASS)
        board_led(LED_RED, 1);
    if (xTaskCreate(scheme_task, "Scheme", SCHEME_STACK_BYTES / sizeof(StackType_t),
                    NULL, (UBaseType_t)PRIORITY_NORMAL, NULL) != pdPASS)
        board_led(LED_RED, 1);      /* not enough FreeRTOS heap for the task stack */
}
