#!/usr/bin/env python3
"""Paste a file into the Qomu Scheme REPL straight through the serial port.

Bypasses screen/terminal/ssh so host-side buffering can be ruled in or out.
Wakes the REPL, reads (fifo-stats) before and after, sends the file with
LF -> CR in chunks of --chunk bytes, and captures all output to a log.

Prints Error and tests: lines, and any line of the form name=value, so a
benchmark can report its numbers that way, e.g. bench.scm's fib20time=905.

Run on the Pi with the terminal program closed:
    python3 tools/paste_test.py scm/tests.scm
    python3 tools/paste_test.py scm/bench.scm --idle 3
"""
import argparse, os, re, select, sys, termios, threading, time, tty

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--chunk", type=int, default=1024, help="bytes per write()")
ap.add_argument("--gap", type=float, default=0.0, help="seconds between writes")
ap.add_argument("--log", default="paste_test.log")
ap.add_argument("--idle", type=float, default=30.0, help="stop after this many quiet seconds")
a = ap.parse_args()

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
termios.tcflush(fd, termios.TCIOFLUSH)

out = bytearray()
last_rx = [time.time()]
stop = [False]

def reader():
    while not stop[0]:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            b = os.read(fd, 4096)
            if b:
                out.extend(b)
                last_rx[0] = time.time()

th = threading.Thread(target=reader, daemon=True)
th.start()

def send(b):
    last_rx[0] = time.time()      # quiet time counts from our own send
    view = memoryview(b)
    while view:
        n = os.write(fd, view)
        view = view[n:]

def wait_quiet(t):
    while time.time() - last_rx[0] < t:
        time.sleep(0.1)

data = open(a.file, "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r")

send(b"\r")                       # wake key (the REPL discards it)
time.sleep(0.5); wait_quiet(1.0)
send(b"(fifo-stats)\r"); wait_quiet(1.0)
t0 = time.time()
for i in range(0, len(data), a.chunk):
    send(data[i:i + a.chunk])
    if a.gap:
        time.sleep(a.gap)
wait_quiet(a.idle)
send(b"(fifo-stats)\r"); wait_quiet(2.0)
stop[0] = True
th.join()
os.close(fd)

open(a.log, "wb").write(out)
text = out.decode("latin-1")
stats = [l for l in text.replace("\r", "").split("\n") if l.startswith('"') and l.count(" ") in (4, 5)]
print(f"sent {len(data)} bytes in chunks of {a.chunk} ({time.time() - t0:.1f} s), log: {a.log}")
if len(stats) >= 2:
    s0 = [int(x) for x in stats[0].strip('"').split()]
    s1 = [int(x) for x in stats[-1].strip('"').split()]
    d = [(y - x) & 0xffff for x, y in zip(s0, s1)]
    print(f"fifo-stats delta: push {d[0]}  pop {d[1]}  full {d[2]}  isr {d[4]}"
          + (f"  irqs {d[5]}" if len(d) > 5 else "")
          + f"   (expected push {len(data) + 13})")
for l in text.replace("\r", "").split("\n"):
    if l.startswith("Error") or l.startswith("tests:") or re.match(r"[A-Za-z][\w-]*=\S", l):
        print("  " + l)
