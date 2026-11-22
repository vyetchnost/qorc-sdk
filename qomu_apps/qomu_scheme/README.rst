Qomu Scheme REPL (Cortex-M4F)
=============================

A small Scheme interpreter running as a FreeRTOS task on the Qomu's
USB-serial console. Built on ``qomu_helloworldsw``: same USB-serial gateware
and startup; the CLI task is replaced by the Scheme task.

Files
-----

- ``src/scheme.c``, ``inc/scheme.h`` - portable interpreter core (no board code)
- ``inc/scheme_port.h`` - FreeRTOS hooks (string/vector bodies from ``pvPortMalloc``)
- ``src/scheme_task.c`` - console I/O over USB serial, board primitives, the task
- ``tests.scm`` - regression tests; paste into the REPL or run on a host build

Build and flash
---------------

::

    cd qomu_apps/qomu_scheme/GCC_Project
    make
    # output/bin/qomu_scheme.bin - flash as you would helloworld

Connect a terminal (e.g. ``screen /dev/ttyACM0 115200`` or
``picocom /dev/ttyACM0``) and press Enter for a ``>`` prompt.

Sizing
------

Set in ``inc/Fw_global_config.h``:

- ``SCHEME_HEAP_BYTES`` (48 KB) - static cell heap, 12 bytes per cell
  (~4000 cells; the base system uses ~900)
- ``SCHEME_STACK_BYTES`` (20 KB) - task stack from the FreeRTOS heap; bounds
  non-tail recursion to roughly 150 levels. Tail calls do not grow the stack.

If the link fails with ``region ram overflowed``, reduce ``SCHEME_HEAP_BYTES``.
Note the FPGA bitstream array (~76 KB) also lives in RAM.

Language
--------

Integers (31-bit immediate, int64 boxed, overflow to float), single-precision
floats, strings, chars, symbols, vectors, closures. Special forms: ``quote``
``quasiquote`` ``lambda`` ``define`` ``set!`` ``if`` ``cond`` (with ``=>``)
``case`` ``and`` ``or`` ``when`` ``unless`` ``let`` (incl. named) ``let*``
``letrec`` ``begin`` ``define-macro``. Library macros: ``do`` ``while``
``dotimes`` ``delay``. Proper tail calls. No continuations, rationals or ports.

Board primitives
----------------

::

    (peek addr) (poke addr val)      ; 32-bit, addr must be aligned
    (peek8 addr) (poke8 addr val)
    (sleep ms) (ms)
    (led 'red|'green|'blue on?)      ; red/green share GPIO0
    (blink 'blue 5 200)
    (gpio-write n v) (gpio-read n)
    (hex n) (room) (gc) (heap-free) (stack-free) (help)

Example::

    > (hex (peek #x40004c18))
    "#x..."
    > (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
    > (fib 20)
    6765

Console
-------

Backspace, Ctrl-U (kill line), up-arrow (recall previous line), Ctrl-C
(interrupt a running program or abandon input).
