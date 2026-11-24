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
 * Console input. The USB-serial core has a 512-byte RX FIFO, no flow control,
 * and drops bytes when full. A pasted block can arrive faster than one FIFO
 * per millisecond, so a polling task is not enough: the core's RX interrupt
 * (FPGA interrupt 0, level-triggered while data is waiting) drains the FIFO
 * into this ring as bytes arrive. Ctrl-C is handled in the ISR too: it
 * discards pending input and raises a flag the interpreter checks.
 */
extern volatile fpga_usbserial_regs_t *pusbserial_regs;   /* defined by the HAL */

static uint8_t rx_ring[SCHEME_RX_BYTES];
static volatile uint32_t rx_head;           /* written by the ISR   */
static volatile uint32_t rx_tail;           /* written by the REPL, and the ISR on ^C */
static volatile int rx_break;
static volatile uint32_t rx_dropped;        /* bytes lost to a full ring */
static volatile uint32_t rx_irqs;           /* ISR entries (diagnostics) */
static volatile uint32_t rx_total;          /* bytes taken from the FIFO (diagnostics) */

/*
 * Bytes the u2m FIFO is guaranteed to hold for each pop-flag value
 * (0 empty, 1 one, 2 >=2, 3 >=4, 4 >=8, 5 >=16, 6 >=32, 8 >=64, D >=1/4,
 * E >=1/2, F full; reserved codes count as 1). Every FPGA register read
 * costs ~2.3 us, so the ISR reads the flags once per batch and then pops
 * that many bytes with one read each, instead of flags + flags + data.
 */
static const uint16_t rx_pop_count[16] = {
    0, 1, 2, 4, 8, 16, 32, 1, 64, 1, 1, 1, 1, 128, 256, 512
};

static void usb_rx_isr(void)
{
    /* Always drain the FIFO: the interrupt is level-triggered and would
     * re-fire forever if bytes were left behind. */
    unsigned n;
    rx_irqs++;
    while ((n = rx_pop_count[pusbserial_regs->u2m_fifo_flags & 0xf]) != 0) {
        while (n--) {
            int c = pusbserial_regs->rdata & 0xff;      /* reading pops the FIFO */
            uint32_t next = (rx_head + 1) % SCHEME_RX_BYTES;
            rx_total++;
            if (c == 3) {
                rx_tail = rx_head;          /* ^C cancels everything still queued */
                rx_break = 1;
            } else if (next == rx_tail) {
                rx_dropped++;
            } else {
                rx_ring[rx_head] = (uint8_t)c;
                rx_head = next;
            }
        }
    }
}

static void usb_rx_init(void)
{
    FB_RegisterISR(FB_INTERRUPT_0, usb_rx_isr);
    FB_ConfigureInterrupt(FB_INTERRUPT_0, FB_INTERRUPT_TYPE_LEVEL,
                          FB_INTERRUPT_POL_LEVEL_HIGH,
                          FB_INTERRUPT_DEST_AP_DISBLE, FB_INTERRUPT_DEST_M4_ENABLE);
    NVIC_ClearPendingIRQ(FbMsg_IRQn);
    NVIC_EnableIRQ(FbMsg_IRQn);             /* priority is set in main.c nvic_init() */
    pusbserial_regs->u2m_fifo_int_en = 1;
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
            int c = -1;
            taskENTER_CRITICAL();           /* the ISR may flush the ring on ^C */
            if (rx_head != rx_tail) {
                c = rx_ring[rx_tail];
                rx_tail = (rx_tail + 1) % SCHEME_RX_BYTES;
            }
            taskEXIT_CRITICAL();
            if (c >= 0) return c;
            continue;
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

/* ---------------------------------------------------------------- diagnostics */

#define DEMCR       (*(volatile uint32_t *)0xE000EDFC)   /* Cortex-M debug block */
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004)

/* USB-serial core registers (see fpga_usbserial_regs_t) */
#define USB_REG(off) ((volatile uint32_t *)((uintptr_t)pusbserial_regs + (off)))
#define USB_DEVICE_ID   0x00
#define USB_U2M_FLAGS   0x40

static void cyccnt_on(void)
{
    DEMCR |= 1u << 24;          /* TRCENA */
    DWT_CTRL |= 1u;             /* CYCCNTENA */
}

/* cycles for n reads of *addr, interrupts masked so the ISR can't skew it */
static uint32_t time_reads(volatile uint32_t *addr, uint32_t n)
{
    uint32_t t0, t1, i, sink = 0;
    taskENTER_CRITICAL();
    t0 = DWT_CYCCNT;
    for (i = 0; i < n; i++)
        sink += *addr;
    t1 = DWT_CYCCNT;
    taskEXIT_CRITICAL();
    (void)sink;
    return t1 - t0;
}

static void put_num(const char *label, double v, const char *unit)
{
    scm_puts(label);
    scm_display(scm_from_num(v));
    scm_puts(unit);
}

/*
 * (bench-fpga-read n) - time n reads of a USB-core register vs SRAM, report
 * the CPU clock and an estimate of the ISR's drain rate. Returns cycles per
 * FPGA register read.
 */
static scm_obj p_bench_fpga_read(scm_obj args)
{
    static volatile uint32_t sram_word;
    int64_t n64 = scm_to_int(scm_arg(&args));
    uint32_t n, c_sram, c_flags, c_id, t0, t1;
    double mhz, per_sram, per_flags, per_id, per_byte;

    if (n64 < 1 || n64 > 100000) scm_error("bench-fpga-read: n must be 1..100000", NULL);
    n = (uint32_t)n64;
    cyccnt_on();

    t0 = DWT_CYCCNT;
    vTaskDelay(pdMS_TO_TICKS(100));
    t1 = DWT_CYCCNT;
    mhz = (double)(t1 - t0) / 100000.0;

    c_sram = time_reads(&sram_word, n);
    c_flags = time_reads(USB_REG(USB_U2M_FLAGS), n);
    c_id = time_reads(USB_REG(USB_DEVICE_ID), n);

    per_sram = (double)c_sram / n;
    per_flags = (double)c_flags / n;
    per_id = (double)c_id / n;
    /* ISR per byte: one flags read + one data read, plus loop/ring overhead */
    per_byte = 2.0 * per_flags + 12.0;

    put_num("CPU clock:          ", mhz, " MHz\n");
    put_num("SRAM read (loop):   ", per_sram, " cycles\n");
    put_num("RX flags read:      ", per_flags, " cycles");
    put_num(" = ", per_flags * 1000.0 / mhz, " ns\n");
    put_num("device_id read:     ", per_id, " cycles\n");
    put_num("est. drain ceiling: ", mhz * 1000.0 / per_byte, " KB/s");
    scm_puts(" (USB full-speed bursts can reach ~1000)\n");
    return scm_from_num(per_flags);
}

/*
 * (usb-rx-probe ms) - stop draining the RX FIFO for ms milliseconds and print
 * every change of the RX status flags, to learn how they encode fill level.
 * Keep what you type or paste during the probe under 512 bytes.
 */
static scm_obj p_usb_rx_probe(scm_obj args)
{
    int64_t ms = scm_to_int(scm_arg(&args));
    uint32_t last = 0xffffffffu, t;
    if (ms < 1 || ms > 60000) scm_error("usb-rx-probe: ms must be 1..60000", NULL);
    scm_puts("RX interrupt off; type or paste now (< 512 bytes)...\n");
    pusbserial_regs->u2m_fifo_int_en = 0;
    for (t = 0; t <= (uint32_t)ms; t++) {
        uint32_t f = *USB_REG(USB_U2M_FLAGS) & 0xf;
        if (f != last) {
            put_num("  t=", (double)t, " ms");
            put_num("  flags=", (double)f, "\n");
            last = f;
        }
        vTaskDelay(1);
    }
    pusbserial_regs->u2m_fifo_int_en = 1;     /* ISR drains what arrived */
    scm_puts("RX interrupt back on.\n");
    return SCM_UNSPEC;
}

/*
 * USB core debug counters (open-toolchain gateware only; FPGA registers 0x14 /
 * 0x18): line transitions, packet starts, valid packets, transmitted packets,
 * each saturating at 65535. The SpDE gateware answers 0xFABDEFAC there.
 */
#define USB_DBG0        0x14
#define USB_DBG1        0x18
#define FPGA_UNDEF_REG  0xFABDEFACu

static int usb_dbg_read(uint32_t *lines, uint32_t *starts, uint32_t *valid, uint32_t *tx)
{
    uint32_t d0 = *USB_REG(USB_DBG0), d1 = *USB_REG(USB_DBG1);
    if (d0 == FPGA_UNDEF_REG)
        return 0;
    *lines = d0 & 0xffff; *starts = d0 >> 16; *valid = d1 & 0xffff; *tx = d1 >> 16;
    return 1;
}

/*
 * Bring-up diagnostics while waiting for the first key: USB progress on the
 * LEDs (below) and a [wait] status line once a second. Off by default; the
 * status lines are echoed back as input unless a terminal already has the
 * port open in raw mode.
 */
#ifndef SCHEME_USB_DIAG
#define SCHEME_USB_DIAG 0
#endif

#if SCHEME_USB_DIAG
/*
 * Show how far USB gets, on the LEDs, while no terminal is attached:
 *   red solid      - D+/D- never change          (pads / input path)
 *   red blinking   - lines toggle, no sync found  (clock recovery / timing)
 *   green blinking - packets start, none valid    (data recovery / CRC)
 *   green solid    - valid packets, nothing sent  (protocol engine)
 *   blue           - the core transmits           (transmit side)
 */
static void usb_dbg_leds(unsigned tick)
{
    uint32_t lines, starts, valid, tx;
    int blink = (tick / 4) & 1;
    if (!usb_dbg_read(&lines, &starts, &valid, &tx)) {
        board_led(LED_GREEN, 1);        /* SpDE gateware: plain "waiting" */
        return;
    }
    board_led(LED_BLUE, tx > 0);
    if (tx > 0)            board_led(LED_GREEN, 0);
    else if (valid > 0)    board_led(LED_GREEN, 1);
    else if (starts > 0)   board_led(LED_GREEN, blink);
    else if (lines > 0)    board_led(LED_RED, blink);
    else                   board_led(LED_RED, 1);
}
#endif /* SCHEME_USB_DIAG */

/* (usb-dbg) -> "lines starts valid tx" counter string, or #f with SpDE gateware */
static scm_obj p_usb_dbg(scm_obj args)
{
    uint32_t v[4];
    char buf[64], *p = buf;
    int i;
    (void)args;
    if (!usb_dbg_read(&v[0], &v[1], &v[2], &v[3]))
        return SCM_FALSE;
    for (i = 0; i < 4; i++) {
        char d[8]; int n = 0; uint32_t x = v[i];
        do { d[n++] = (char)('0' + x % 10); x /= 10; } while (x);
        if (i) *p++ = ' ';
        while (n) *p++ = d[--n];
    }
    *p = 0;
    return scm_from_cstr(buf);
}

/* (fifo-stats) -> "push pop full m2u isr": gateware byte counters along the
 * serial path (16-bit, wrapping) and the firmware's own count of bytes taken
 * by the RX interrupt (low 16 bits). #f with gateware that lacks them. */
#define USB_DBG2 0x1C
#define USB_DBG3 0x20
static scm_obj p_fifo_stats(scm_obj args)
{
    uint32_t d2 = *USB_REG(USB_DBG2), d3 = *USB_REG(USB_DBG3), v[5];
    char buf[64], *p = buf;
    int i;
    (void)args;
    if (d2 == FPGA_UNDEF_REG) return SCM_FALSE;
    v[0] = d2 & 0xffff; v[1] = d2 >> 16; v[2] = d3 & 0xffff; v[3] = d3 >> 16;
    v[4] = rx_total & 0xffff;
    for (i = 0; i < 5; i++) {
        char d[8]; int n = 0; uint32_t x = v[i];
        do { d[n++] = (char)('0' + x % 10); x /= 10; } while (x);
        if (i) *p++ = ' ';
        while (n) *p++ = d[--n];
    }
    *p = 0;
    return scm_from_cstr(buf);
}

/* (rx-dropped) -> console input bytes lost because the input ring was full */
static scm_obj p_rx_dropped(scm_obj args)
{
    (void)args;
    return scm_from_int((int64_t)rx_dropped);
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
"  (display \"  (hex n) (room) (gc) (heap-free) (stack-free) (rx-dropped)\\n\")\n"
"  (display \"Ctrl-C interrupts a running program; up-arrow recalls the last line.\\n\"))\n";

/* ---------------------------------------------------------------- task */

/* Bring-up diagnostics while waiting for the first key: one line a second
 * with both FIFO flag nibbles, the RX interrupt enable and count, bytes in
 * the input ring and the gateware USB counters. Seeing these lines at all
 * proves M4->USB works; typing should move u2m (FIFO) and then irq/ring. */
#if SCHEME_USB_DIAG
static void put_hex(uint32_t v, int digits)
{
    while (digits--) q_putc("0123456789abcdef"[(v >> (4 * digits)) & 0xf]);
}

static void put_str(const char *t)
{
    while (*t) q_putc(*t++);
}

static void wait_status_line(void)
{
    uint32_t l = 0, st = 0, v = 0, t = 0;
    put_str("[wait] u2m=");  put_hex(pusbserial_regs->u2m_fifo_flags, 1);
    put_str(" m2u=");        put_hex(pusbserial_regs->m2u_fifo_flags, 1);
    put_str(" ien=");        put_hex(pusbserial_regs->u2m_fifo_int_en & 1, 1);
    put_str(" irqs=");       put_hex(rx_irqs, 4);
    put_str(" ring=");       put_hex((rx_head - rx_tail) % SCHEME_RX_BYTES, 4);
    if (usb_dbg_read(&l, &st, &v, &t)) {
        put_str(" usb l/s/v/t=");
        put_hex(l, 4); q_putc('/'); put_hex(st, 4); q_putc('/');
        put_hex(v, 4); q_putc('/'); put_hex(t, 4);
    }
    put_str("\r\n");
}
#endif /* SCHEME_USB_DIAG */

static void scheme_task(void *arg)
{
    static const scm_io io = { q_putc, q_getc, q_avail, 1, q_brk };
    (void)arg;

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
    scm_defprim("rx-dropped", p_rx_dropped, 0, 0);
    scm_defprim("bench-fpga-read", p_bench_fpga_read, 1, 1);
    scm_defprim("usb-rx-probe", p_usb_rx_probe, 1, 1);
    scm_defprim("usb-dbg", p_usb_dbg, 0, 0);
    scm_defprim("fifo-stats", p_fifo_stats, 0, 0);
#if INCLUDE_uxTaskGetStackHighWaterMark
    scm_defprim("stack-free", p_stack_free, 0, 0);
#endif
    scm_eval_cstr(board_prelude);

    /* Green while waiting: the banner prints once someone is typing, i.e. a
     * terminal is open. All LEDs go off when the REPL starts. */
    board_led(LED_GREEN, 1);
    {
#if SCHEME_USB_DIAG
        unsigned tick = 0;
#endif
        while (!q_avail()) {
#if SCHEME_USB_DIAG
            usb_dbg_leds(tick++);
            if (tick % 20 == 0)
                wait_status_line();
#endif
            vTaskDelay(50);
        }
    }
    board_led(LED_RED, 0);
    board_led(LED_GREEN, 0);
    board_led(LED_BLUE, 0);

    /* The host side can send stray bytes when the port is opened; let them
     * arrive, then drop them together with the wake-up key. */
    vTaskDelay(150);
    taskENTER_CRITICAL();
    rx_tail = rx_head;
    rx_break = 0;
    taskEXIT_CRITICAL();

    scm_puts("\nQomu Scheme - ");
    scm_display(scm_from_int((int64_t)scm_free_cells()));
    scm_puts(" cells free. Type (help) for board commands.\n");

    for (;;)
        scm_repl();         /* only returns on EOF, which the tty never sends */
}

void scheme_start_task(void)
{
    usb_rx_init();
    if (xTaskCreate(scheme_task, "Scheme", SCHEME_STACK_BYTES / sizeof(StackType_t),
                    NULL, (UBaseType_t)PRIORITY_NORMAL, NULL) != pdPASS)
        board_led(LED_RED, 1);      /* not enough FreeRTOS heap for the task stack */
}
