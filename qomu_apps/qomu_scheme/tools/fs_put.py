#!/usr/bin/env python3
"""Save a local file into Qomu flash storage under a name.

Sends (write-file "NAME"), the file's text, and the "." terminator line,
then prints what the board answered ("Saved NAME: N bytes" or an Error).
A line in the file holding only "." would end the capture early, so the
tool refuses such files.

Run on the Pi with the terminal program closed:
    python3 tools/fs_put.py boot.scm            # saved as "boot"
    python3 tools/fs_put.py lib.scm --name mylib
"""
import argparse, os, select, sys, termios, time, tty

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--name", help="flash file name (default: file name without extension)")
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--timeout", type=float, default=20.0)
a = ap.parse_args()

name = a.name or os.path.splitext(os.path.basename(a.file))[0]
text = open(a.file, "rb").read().replace(b"\r\n", b"\n")
if any(line == b"." for line in text.split(b"\n")):
    sys.exit("file has a line holding only '.', which would end the capture")
if len(text) > 65280:
    sys.exit("file is %d bytes; a flash file holds at most 65280" % len(text))
if not text.endswith(b"\n"):
    text += b"\n"

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
termios.tcflush(fd, termios.TCIOFLUSH)

def send(b):
    view = memoryview(b)
    while view:
        view = view[os.write(fd, view):]

def read_until(pred, timeout):
    buf, end = bytearray(), time.time() + timeout
    while time.time() < end and not pred(buf):
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            buf.extend(os.read(fd, 4096))
    return bytes(buf)

send(b"\r")                                     # wake key (discarded by the REPL)
read_until(lambda b: False, 0.6)
send(b"\x15\r")                                 # ^U: clear any half-typed line
read_until(lambda b: b.endswith(b"> "), 2.0)
send(b'(write-file "%s")\r' % name.encode())
got = read_until(lambda b: b"Ctrl-C cancels" in b or b"Error" in b, 5.0)
if b"Error" in got:
    sys.exit(got.decode(errors="replace").strip())
send(text.replace(b"\n", b"\r") + b".\r")
got = read_until(lambda b: (b"Saved" in b or b"Error" in b) and b.endswith(b"> "), a.timeout)
for line in got.decode(errors="replace").replace("\r", "").split("\n"):
    if line.startswith("Saved") or "Error" in line:
        print(line)
        sys.exit(0 if line.startswith("Saved") else 1)
sys.exit("no answer from the board:\n" + got.decode(errors="replace"))
