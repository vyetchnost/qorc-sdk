#!/usr/bin/env python3
"""Qomu Scheme: USB serial from C struct to D+/D- (hand-laid SVG).

    python3 tools/usb_serial_map.py            # writes ../Qomu USB Serial Map.svg
    rsvg-convert "../Qomu USB Serial Map.svg" -o map.png   # optional PNG
"""
import os, sys
from xml.sax.saxutils import escape

W, H = 2440, 1745
out = []
def a(s): out.append(s)

SANS = "Inter, 'Helvetica Neue', Helvetica, 'DejaVu Sans', Arial, sans-serif"
MONO = "'DejaVu Sans Mono', Menlo, monospace"   # Menlo shares DejaVu Mono's metrics
FG, MUTED, LINE = "#18211d", "#5a6761", "#c9d1cc"
HOST, FAB, ACC, RED = "#3a5aa0", "#a8620a", "#0f6e5a", "#b3362d"
TX, RX = "#d2561b", "#1f6fd1"           # data path colours
D12, D48 = "#fbf1e1", "#efe7f8"         # clock-domain fills

def rect(x, y, w, h, fill="#fff", stroke=LINE, sw=1.2, r=8, dash=None, extra=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d} {extra}/>')

def text(x, y, s, size=13, fill=FG, weight=400, font=SANS, anchor="start", italic=False):
    st = ' font-style="italic"' if italic else ""
    a(f'<text x="{x}" y="{y}" font-family="{font}" font-size="{size}" fill="{fill}" '
      f'font-weight="{weight}" text-anchor="{anchor}"{st}>{escape(s)}</text>')

def lines(x, y, rows, size=12.5, lh=None, font=SANS, fill=FG):
    lh = lh or size * 1.42
    for i, r in enumerate(rows):
        if isinstance(r, tuple):
            s, kw = r[0], r[1]
            text(x, y + i * lh, s, size=kw.get("size", size), fill=kw.get("fill", fill),
                 weight=kw.get("weight", 400), font=kw.get("font", font))
        else:
            text(x, y + i * lh, r, size=size, font=font, fill=fill)
    return y + len(rows) * lh

def box(x, y, w, h, title, sub=None, color=FAB, fill="#fff", body=None, mono=False, bsize=12):
    rect(x, y, w, h, fill=fill, stroke=color, sw=1.6)
    a(f'<rect x="{x}" y="{y}" width="6" height="{h}" rx="3" fill="{color}"/>')
    text(x + 16, y + 22, title, size=14.5, weight=700, fill=color)
    yy = y + 22
    if sub:
        text(x + 16, y + 40, sub, size=11.5, fill=MUTED, font=MONO)
        yy = y + 40
    if body:
        lines(x + 16, yy + 21, body, size=bsize, font=MONO if mono else SANS)

def arrow(pts, color=MUTED, sw=2, head=True, dash=None, both=False, label=None, lpos=None, lsize=11.5, lcolor=None):
    d = "M " + " L ".join(f"{px},{py}" for px, py in pts)
    mk = f'marker-end="url(#ah-{color[1:]})"' if head else ""
    ms = f'marker-start="url(#as-{color[1:]})"' if both else ""
    da = f' stroke-dasharray="{dash}"' if dash else ""
    a(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}" {mk} {ms}{da} stroke-linejoin="round"/>')
    if label:
        lx, ly = lpos
        text(lx, ly, label, size=lsize, fill=lcolor or color, font=MONO)

def badge(x, y, n, color):
    a(f'<circle cx="{x}" cy="{y}" r="11" fill="{color}"/>')
    text(x, y + 4.5, str(n), size=13, fill="#fff", weight=700, anchor="middle")

def region(x, y, w, h, label, sub, color):
    rect(x, y, w, h, fill="none", stroke=color, sw=2, r=14, dash="7 5")
    a(f'<rect x="{x+18}" y="{y-13}" width="{len(label)*9.2+24}" height="26" rx="13" fill="{color}"/>')
    text(x + 30, y + 5, label, size=14, fill="#fff", weight=700)
    text(x + 18, y + 34, sub, size=12, fill=MUTED, italic=True)

# ------------------------------------------------------------------ header
a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
a('<defs>')
for c in [MUTED, TX, RX, HOST, FAB, ACC, RED, FG, "#7a4fb5"]:
    k = c[1:]
    a(f'<marker id="ah-{k}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>')
    a(f'<marker id="as-{k}" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>')
a('</defs>')
a(f'<rect width="{W}" height="{H}" fill="#f4f6f4"/>')
text(40, 52, "Qomu Scheme: USB serial, from a C struct to the wires", size=30, weight=700)
text(40, 80, "What the firmware sees (left), the EOS S3's fixed hardware, the gateware we build into the eFPGA, "
     "and the pins on the board. Numbered dots follow one byte each way.", size=15, fill=MUTED)

# legend
lx = 1500
a(f'<circle cx="{lx}" cy="47" r="9" fill="{TX}"/>'); text(lx + 16, 52, "M4 → host (output, e.g. a REPL answer)", size=13)
a(f'<circle cx="{lx}" cy="73" r="9" fill="{RX}"/>'); text(lx + 16, 78, "host → M4 (typing, pastes)", size=13)
rect(lx + 300, 38, 26, 18, fill=D12, stroke=FAB, r=4); text(lx + 334, 52, "12 MHz domain (Sys_Clk0)", size=13)
rect(lx + 300, 64, 26, 18, fill=D48, stroke="#7a4fb5", r=4); text(lx + 334, 78, "48 MHz domain (Sys_Clk1)", size=13)
a(f'<line x1="{lx+560}" y1="47" x2="{lx+600}" y2="47" stroke="{MUTED}" stroke-width="2" stroke-dasharray="6 4"/>')
text(lx + 608, 52, "set-up only", size=13)
a(f'<line x1="{lx+560}" y1="73" x2="{lx+600}" y2="73" stroke="{HOST}" stroke-width="6"/>')
text(lx + 608, 78, "bus", size=13)

# ------------------------------------------------------------------ region A: firmware
AX, AY, AW, AH = 30, 120, 450, 1570
region(AX, AY, AW, AH, "M4 firmware (C)", "qomu_scheme/src: what the code touches", HOST)

box(50, 175, 410, 118, "Scheme REPL task", "scheme.c · FreeRTOS, 48 MHz M4", HOST, body=[
    "print → outc() → q_putc(c)",
    "reader ← q_getc() ← 8 KB input ring",
    "Ctrl-C: the ISR flushes the ring, sets rx_break"])

box(50, 310, 410, 250, "scheme_task.c: the two hot paths", None, HOST, mono=True, bsize=11.5, body=[
    ("q_putc(c):", {"weight": 700, "font": MONO, "fill": TX}),
    "  while (m2u_fifo_flags == 0)  // full",
    "      wait, give up after ~100 ms",
    "  pusbserial_regs->wdata = c;",
    "",
    ("usb_rx_isr():   // FbMsg_IRQn", {"weight": 700, "font": MONO, "fill": RX}),
    "  f = u2m_fifo_flags;      // fill level",
    "  n = rx_pop_count[f];     // safe count",
    "  while (n--) ring[head++] = rdata;",
    "  // each rdata read pops one byte"])
badge(440, 349, 1, TX)
badge(440, 431, 6, RX)

box(50, 578, 410, 520, "fpga_usbserial_regs_t", "freertos_gateware/inc/eoss3_hal_fpga_usbserial.h", HOST, mono=True, bsize=11.5, body=[
    "volatile fpga_usbserial_regs_t *pusbserial_regs",
    "    = (…)FPGA_PERIPH_BASE;    // 0x40020000",
    "",
    "off   member              use",
    "0x00  device_id           0xA5BD, checked",
    "0x04  rev_num             0x0200",
    "0x08  scratch_reg         free R/W",
    "0x0C  clock_select        0 (old ÷1.5 option)",
    "0x10  usbpid              0x6141 → descriptor",
    "0x14  (reserved2[0..3])   our debug counters,",
    " ..0x20                   read via raw pointers",
    "0x40  u2m_fifo_flags :4   bytes waiting (level)",
    "0x44  rdata :8            READ = pop one byte",
    "0x80  m2u_fifo_flags :4   room left (level)",
    "0x84  wdata :8            WRITE = push one byte",
    "0xC0  u2m_fifo_int_en :1  RX interrupt on",
    "",
    "Bit-fields over 32-bit words: every member is",
    "one aligned 32-bit load or store on the bus.",
    "Reading rdata has a side effect, so a debugger",
    "watch on it steals bytes."])

box(50, 1116, 410, 300, "Bring-up, once (main.c, HAL)", None, HOST, mono=True, bsize=11.5, body=[
    "load_fpga(qomu_fpga.c)     // bitstream",
    "qomu_fpga_pads_init()      // IO mux",
    "HAL_usbserial_init2(false, false, 0x6141):",
    "  C16 = 12 MHz, enable     // Sys_Clk0",
    "  clock_select = 0",
    "  usbpid = 0x6141",
    "  C21 = 48 MHz, enable     // Sys_Clk1",
    "  assert device_id == 0xA5BD",
    "usb_rx_init():",
    "  u2m_fifo_int_en = 1",
    "  NVIC_EnableIRQ(FbMsg_IRQn)  // prio 5",
    ])

box(50, 1434, 410, 236, "Diagnostics we added", None, HOST, mono=True, bsize=11.5, body=[
    "(fifo-stats) push pop full m2u isr irqs",
    "   FIFO byte counters @0x1C/0x20 + ISR counts",
    "(usb-dbg)   datatx bad valid acks",
    "   packet counters @0x14/0x18",
    "(bench-fpga-read n)",
    "   one register read ≈ 112 cycles = 2.33 µs",
    "",
    "The bus is slow: that's why the ISR pops a",
    "batch per interrupt, not one byte."])

# ------------------------------------------------------------------ region B: EOS S3 fixed hardware
BX, BY, BW, BH = 510, 120, 360, 1570
region(BX, BY, BW, BH, "EOS S3 fixed hardware", "the chip's ASIC side, not reprogrammable", ACC)

box(530, 175, 320, 96, "Cortex-M4F", "C10 = 48 MHz (HSOSC)", ACC, body=[
    "runs FreeRTOS + Scheme from SRAM",
    "loads/stores to 0x4002_0000 window"])

box(530, 300, 320, 230, "AHB → Wishbone bridge", "inside qlal4s3b_cell_macro", ACC, body=[
    "maps 0x4002_0000–0x4003_FFFF (128 KB)",
    "to the fabric as a Wishbone master:",
    ("  WBs_ADR[16:0]  WBs_BYTE_STB[3:0]", {"font": MONO}),
    ("  WBs_WR_DAT / WBs_RD_DAT [31:0]", {"font": MONO}),
    ("  WBs_CYC STB WE RD → ACK", {"font": MONO}),
    "clocked by WB_CLK = Sys_Clk0 (12 MHz):",
    "the M4 stalls until the fabric ACKs"])
badge(800, 322, 2, TX); badge(828, 322, 6, RX)

box(530, 560, 320, 120, "Interrupt controller", None, ACC, body=[
    ("FB_msg_out[0] ← Interrupt_o", {"font": MONO}),
    ("  → FbMsg_IRQn (NVIC)", {"font": MONO}),
    "level: stays high while the u2m FIFO",
    "has bytes and int_en = 1"])
badge(828, 582, 5, RX)

box(530, 710, 320, 236, "Clock & reset control", "S3x_Clk_* driver, s3x_pwrcfg.c", ACC, body=[
    "HSOSC 48 MHz, all clocks divide from it",
    ("  C10  48 MHz → M4 core", {"font": MONO}),
    ("  C16  12 MHz → Sys_Clk0", {"font": MONO}),
    ("  C21  48 MHz → Sys_Clk1", {"font": MONO}),
    ("  Sys_Clk0_Rst, Sys_Clk1_Rst", {"font": MONO}),
    "the fabric has no oscillator of its own:",
    "change C16/C21 and the gateware's timing",
    "changes with it (the 72 MHz lesson)"])

box(530, 976, 320, 120, "FPGA configuration controller", None, ACC, body=[
    "load_fpga() streams the bitstream",
    "(fpga/qomu_fpga.c, 18,990 words, from",
    "the open toolchain) into the fabric"])

box(530, 1126, 320, 120, "IO mux / pads", None, ACC, body=[
    "qomu_fpga_pads_init() hands balls",
    "E4, F3, E2 to the fabric, with the",
    "pad modes the pcf/iomux script chose"])

box(530, 1276, 320, 150, "Not on this path", None, MUTED, body=[
    "No DMA, no shared memory, no USB PHY:",
    "the S3 has no USB hardware at all. Every",
    "USB bit is made by gateware, and the M4",
    "only ever sees two byte queues."])

# ------------------------------------------------------------------ region C: eFPGA fabric
CX, CY, CW, CH = 900, 120, 1150, 1570
region(CX, CY, CW, CH, "eFPGA fabric: AL4S3B_FPGA_top", "gateware/usb2serial/rtl · Yosys + VPR (ql_symbiflow) · 48 MHz path meets timing at 20.51 ns of 20.83", FAB)

# 12 MHz domain backdrop (most of the fabric) and 48 MHz island
rect(915, 170, 1120, 1505, fill=D12, stroke="none", r=12)
rect(1630, 1185, 395, 300, fill=D48, stroke="#7a4fb5", sw=1.4, r=10, dash="5 4")
text(1645, 1205, "48 MHz domain", size=12.5, fill="#7a4fb5", weight=700)

# clock buffers
box(930, 185, 330, 118, "Global clock buffers (gclkbuff)", "AL4S3B_FPGA_Top.v", FAB, body=[
    ("Sys_Clk0 → clk_12mhz  (also WB_CLK)", {"font": MONO, "size": 11.5}),
    ("Sys_Clk1 → clk_48mhz", {"font": MONO, "size": 11.5}),
    ("Sys_Clk0_Rst | WB_RST, Sys_Clk1_Rst → reset", {"font": MONO, "size": 11})])

# register block
box(930, 330, 330, 470, "AL4S3B_FPGA_Registers", "Wishbone slave · window 0x000–0x1FF", FAB, mono=True, bsize=11.3, body=[
    "decode WBs_ADR, ACK every cycle",
    "",
    "0x00 ID   0x04 REV   0x08 SCRATCH",
    "0x0C clk_sel     0x10 usbpid → EP0",
    "0x14..0x20 debug counters (DBG0-3)",
    "0x40 u2m flags ← popflag[3:0]",
    "0x44 rdata   ← u2m Dout",
    "     read ⇒ FIFO_u2m_pop",
    "0x80 m2u flags ← pushflag[3:0]",
    "0x84 wdata   → m2u Din",
    "     write ⇒ FIFO_m2u_push",
    "0xC0 int_en",
    "other ⇒ 0xFAB_DEF_AC",
    "",
    "Interrupt_o =",
    "   !u2m_empty && int_en",
    "",
    "(upstream popped on the wrong",
    " address; fixed in part two)"])
badge(1238, 352, 3, TX)

# FIFOs
box(930, 830, 330, 175, "FIFO_m2u  512 × 8", "FPGA_FIFO → f512x8_512x8", FAB, body=[
    "M4 → USB. Built on a RAM_8K_BLK",
    "(first-word fall-through; the hard",
    "FIFO mode didn't move data)",
    ("pushflag: room left, 4-bit level", {"font": MONO, "size": 11.3}),
    ("popflag/empty → bridge", {"font": MONO, "size": 11.3})])
box(930, 1030, 330, 175, "FIFO_u2m  512 × 8", "FPGA_FIFO → f512x8_512x8", FAB, body=[
    "USB → M4. Same RAM-based FIFO.",
    ("popflag → u2m_fifo_flags (level", {"font": MONO, "size": 11.3}),
    ("  the ISR turns into a pop count)", {"font": MONO, "size": 11.3}),
    ("pushflag → bridge: room check", {"font": MONO, "size": 11.3}),
    "both FIFOs on clk_12mhz"])
badge(1238, 1052, 4, RX)

# FIFO byte counters
box(930, 1230, 330, 100, "FIFO byte counters", "AL4S3B_FPGA_IP.v (ours)", FAB, body=[
    "count every push and pop → 0x1C/0x20,",
    "so lost bytes can be located end to end"])

# usb2m4_serial wrapper
rect(1290, 185, 745, 1490, fill="none", stroke=FAB, sw=1.4, r=12, dash="3 3")
text(1306, 207, "usb2m4_serial  (the USB device, all of it)", size=13.5, fill=FAB, weight=700)

box(1305, 225, 305, 300, "usb_m4_bridge_ep", "EP1 bulk OUT + IN, 32-byte packets", FAB, body=[
    "OUT: take bytes from the OUT endpoint",
    "buffer → FIFO_u2m_push, but only while",
    ("u2m_room_ok (pushflag not near full)", {"font": MONO, "size": 11.3}),
    "  else stop draining: the packet stays",
    "  in the buffer and the host gets NAK",
    "  ⇒ lossless pastes (part two)",
    "IN: FIFO_m2u_pop → IN endpoint buffer,",
    "send what's there as one packet",
    "",
    "this is where 'two byte queues'",
    "become USB packets"])
badge(1560, 247, 4, TX); badge(1588, 247, 4, RX)

box(1305, 545, 305, 300, "usb_serial_ctrl_ep", "EP0 control: enumeration", FAB, body=[
    "SETUP handling: SET_ADDRESS,",
    "GET_DESCRIPTOR, SET_CONFIGURATION,",
    "CDC class requests (line coding",
    "accepted; baud rate means nothing)",
    "descriptor ROM (ep_rom):",
    ("  device 1d50:<usbpid>", {"font": MONO, "size": 11.3}),
    ("  CDC-ACM: EP2 IN interrupt (8 B),", {"font": MONO, "size": 11.3}),
    ("           EP1 OUT/IN bulk (32 B)", {"font": MONO, "size": 11.3}),
    "the host sees a standard modem:",
    "Linux cdc_acm, no driver needed"])

box(1305, 865, 305, 150, "usb_fs_out_arb / usb_fs_in_arb", None, FAB, body=[
    "share the protocol engine between",
    "EP0 (control) and EP1 (serial):",
    "each endpoint requests, one is",
    "granted the shared buffers"])

box(1305, 1035, 305, 135, "Counters (dbg_counters)", "usb_fs_pe.v (ours)", FAB, body=[
    "datatx · bad · valid · acks",
    "→ DBG0/DBG1 registers → (usb-dbg)",
    "0 bad, ACKs = data at v0.2"])

# protocol engine
rect(1625, 225, 400, 1268, fill="none", stroke=FAB, sw=1.2, r=10)
text(1640, 246, "usb_fs_pe: protocol engine", size=13.5, fill=FAB, weight=700)

box(1640, 262, 370, 205, "usb_fs_out_pe", "OUT / SETUP tokens", FAB, body=[
    "token for this address + endpoint →",
    "store DATA0/1 in an endpoint buffer",
    ("(r512x8_512x8, a RAM_8K_BLK)", {"font": MONO, "size": 11.3}),
    "good CRC + buffer free → ACK",
    "buffer still undrained → NAK",
    "data toggle tracks retries"])
badge(1988, 284, 3, RX)

box(1640, 482, 370, 185, "usb_fs_in_pe", "IN tokens", FAB, body=[
    "IN token → send DATA0/1 from the",
    "IN endpoint buffer (r512x8)",
    "nothing ready → NAK",
    "wait for the host's ACK, else resend",
    "(fast reply: 8 bit times, part four)"])
badge(1988, 504, 5, TX)

box(1640, 682, 370, 72, "usb_fs_tx_mux", None, FAB, body=[
    "picks out_pe handshakes or in_pe data"])

box(1640, 769, 370, 260, "usb_fs_tx", "transmitter · clk_12mhz, 1 bit per clock", FAB, body=[
    "SYNC · PID · data · CRC16 · EOP",
    "bit stuffing (a 0 after six 1s)",
    "NRZI: 0 = toggle the line, 1 = hold",
    ("→ usb_p_tx, usb_n_tx, usb_tx_en", {"font": MONO, "size": 11.3}),
    "ours: moved from 48 to 12 MHz;",
    "starts at once (was 7 idle bits);",
    "holds J (D+ high) while off, so the",
    "first SYNC isn't inverted"])
badge(1988, 791, 5, TX)

box(1640, 1045, 370, 120, "Clock-domain crossing", "strobe.v / strobe_out4.v", "#7a4fb5", body=[
    "48 → 12 MHz event strobes, toggle",
    "synchronizers; fixed to compare only",
    "synchronized stages (part four)"])

box(1645, 1218, 360, 255, "usb_fs_rx", "receiver · clk_48mhz, 4 samples per bit", "#7a4fb5", body=[
    "line state each sample: J, K, SE0",
    "bit phase locks to every transition",
    "NRZI decode, drop stuffed bits",
    "SYNC → PID → bytes → CRC5/CRC16",
    "packet start/end, valid/bad",
    "→ strobes into the 12 MHz domain",
    "ours: boundary detector reworked",
    "for timing (22.6 → 20.5 ns)"])
badge(1983, 1240, 2, RX)

# pad logic
box(1305, 1195, 305, 290, "Pad logic", "AL4S3B_FPGA_IP.v", FAB, mono=True, bsize=11.3, body=[
    "usbp_io = usb_tx_en ? usb_p_tx : Z",
    "usbn_io = usb_tx_en ? usb_n_tx : Z",
    "",
    "usb_p_rx = usb_tx_en ? 1 : usbp_io",
    "usb_n_rx = usb_tx_en ? 0 : usbn_io",
    "  (receiver sees idle J while we",
    "   transmit: no echo of our own)",
    "",
    "usb_pu_cntrl_o = 1",
    "  (always attached)"])

box(1305, 1530, 720, 135, "Half duplex on two wires", None, FAB, body=[
    "D+ and D− carry one differential signal, one direction at a time: the host",
    "speaks (token, maybe data), then the device answers within ~16 bit times or the",
    "host gives up. The FPGA only drives the pins during its own answer; the rest",
    "of the time they're inputs. Full speed = 12 Mbit/s, 1 ms frames.",
    ])

# ------------------------------------------------------------------ region D: board + host
DX, DY, DW, DH = 2080, 120, 335, 1570
region(DX, DY, DW, DH, "Qomu board → host", "Qomu schematic, qomu.pcf", HOST)

box(2100, 175, 295, 140, "Ball E4 · FBIO_31", "usbp_io", HOST, body=[
    ("D+", {"weight": 700, "size": 16}),
    "22 Ω series → USB D+"])
box(2100, 335, 295, 140, "Ball F3 · FBIO_28", "usbn_io", HOST, body=[
    ("D−", {"weight": 700, "size": 16}),
    "22 Ω series → USB D−"])
box(2100, 495, 295, 175, "Ball E2 · FBIO_23", "usb_pu_cntrl_o", HOST, body=[
    ("D+ pull-up control", {"weight": 700, "size": 14}),
    "PU_CTRL_USBP → R9 1.5 kΩ → D+",
    "D+ pulled high = 'full-speed",
    "device attached'; the host",
    "then resets and enumerates"])
badge(2345, 197, 6, TX); badge(2373, 197, 1, RX)

box(2100, 690, 295, 120, "USB plug (the board is the plug)", None, HOST, body=[
    "VBUS → 3.3 V LDO (MIC5504)",
    "GND",
    "D+, D− to the host"])

box(2100, 830, 295, 300, "Host (Pi 4 / Mac)", None, HOST, body=[
    "enumerates 1d50:6141, CDC-ACM",
    "Linux cdc_acm → /dev/ttyACM0",
    "polls IN about every frame;",
    "sends OUT when you type or paste",
    "",
    ("picocom -b 115200 /dev/ttyACM0", {"font": MONO, "size": 11.3}),
    ("tools/paste_test.py", {"font": MONO, "size": 11.3}),
    ("tools/fs_put.py", {"font": MONO, "size": 11.3}),
    "(the 115200 is ignored: the real",
    "rate is whatever USB delivers)"])

box(2100, 1150, 295, 250, "Why these balls", None, HOST, body=[
    "The S3 has no USB PHY, so USB is",
    "just two general-purpose fabric",
    "pins driven at 3.3 V and sampled by",
    "the receiver. FBIO_8/9 nearby are",
    "HSOSC bootstrap pins and are left",
    "alone.",
    "qomu.pcf: usbp_io E4, usbn_io F3,",
    "usb_pu_cntrl_o E2"])

# ------------------------------------------------------------------ connections
# M4 <-> bridge (bus)
a(f'<line x1="690" y1="271" x2="690" y2="300" stroke="{HOST}" stroke-width="6"/>')
# firmware -> M4 (code runs there): dashed
arrow([(460, 234), (530, 234)], color=HOST, sw=2, label="", lpos=(0, 0))
# bridge <-> register block: Wishbone bus
a(f'<line x1="850" y1="420" x2="930" y2="420" stroke="{HOST}" stroke-width="6"/>')
text(856, 410, "Wishbone", size=11, fill=HOST, weight=700)
# struct -> bridge (address window)
arrow([(460, 640), (500, 640), (500, 470), (530, 470)], color=HOST, sw=1.6, dash="5 4")
text(462, 656, "0x40020000", size=10.5, fill=HOST, font=MONO)

# register -> m2u FIFO (push), u2m FIFO -> register (pop)
arrow([(1000, 800), (1000, 830)], color=TX, sw=2.4)
text(1008, 820, "wdata push", size=11, fill=TX, font=MONO)
arrow([(1200, 1030), (1200, 1010), (1250, 1010), (1250, 812), (1200, 812), (1200, 800)], color=RX, sw=2.4)
text(1150, 1022, "rdata pop", size=11, fill=RX, font=MONO, anchor="end")

# m2u FIFO -> bridge (pop into IN)
arrow([(1260, 900), (1280, 900), (1280, 470), (1305, 470)], color=TX, sw=2.4)
text(1268, 892, "pop", size=11, fill=TX, font=MONO, anchor="end")
# bridge -> u2m FIFO (push)
arrow([(1305, 500), (1290, 500), (1290, 1100), (1260, 1100)], color=RX, sw=2.4)
text(1268, 1092, "push", size=11, fill=RX, font=MONO, anchor="end")

# usbpid register -> ctrl ep
arrow([(1230, 395), (1272, 395), (1272, 640), (1305, 640)], color=FAB, sw=1.6, dash="4 3")
text(1240, 655, "usbpid", size=10.5, fill=FAB, font=MONO)

# Interrupt_o -> interrupt controller
arrow([(930, 680), (880, 680), (880, 620), (850, 620)], color=RX, sw=2.2)
text(884, 698, "Interrupt_o", size=10.5, fill=RX, font=MONO)
# interrupt controller -> M4
arrow([(530, 600), (515, 600), (515, 250), (530, 250)], color=RX, sw=2.2)

# clocks: controller -> gclkbuff
arrow([(850, 780), (890, 780), (890, 250), (930, 250)], color="#7a4fb5", sw=2)
text(856, 772, "Sys_Clk0/1", size=10.5, fill="#7a4fb5", font=MONO)

# config controller -> fabric (dashed)
arrow([(850, 1036), (915, 1036)], color=MUTED, sw=1.8, dash="6 4")
text(856, 1028, "bitstream", size=10.5, fill=MUTED, font=MONO)
# IO mux -> pins (dashed, under the fabric)
arrow([(850, 1186), (880, 1186), (880, 1712), (2064, 1712), (2064, 600), (2100, 600)], color=MUTED, sw=1.6, dash="6 4", head=True)
text(1000, 1730, "IO mux assigns balls E2, E4, F3 to the fabric (qomu_fpga_pads_init)", size=11, fill=MUTED, font=MONO)

# bridge <-> arbiters <-> pe
arrow([(1640, 360), (1610, 360)], color=RX, sw=2.4)       # out_pe -> bridge (data_get)
text(1614, 352, "get", size=10, fill=RX, font=MONO)
arrow([(1610, 510), (1640, 510)], color=TX, sw=2.4)       # bridge -> in_pe (data_put)
text(1614, 502, "put", size=10, fill=TX, font=MONO)
# ctrl ep <-> pe (both ways)
arrow([(1610, 600), (1625, 600), (1625, 420), (1640, 420)], color=FAB, sw=1.6, both=True)
arrow([(1610, 700), (1632, 700), (1632, 600), (1640, 600)], color=FAB, sw=1.6, both=True)
# in_pe -> tx_mux -> tx
arrow([(1825, 667), (1825, 682)], color=TX, sw=2.4)
arrow([(1825, 754), (1825, 769)], color=TX, sw=2.4)
# out_pe handshake -> tx_mux
arrow([(1990, 467), (1990, 718), (2010, 718)], color=FAB, sw=1.2, head=False)
# tx -> pad logic
arrow([(1640, 950), (1620, 950), (1620, 1250), (1610, 1250)], color=TX, sw=2.4)
text(1478, 1235, "p_tx n_tx tx_en", size=10.5, fill=TX, font=MONO)
# pad logic -> rx
arrow([(1610, 1400), (1645, 1400)], color=RX, sw=2.4)
text(1500, 1395, "p_rx n_rx", size=10.5, fill=RX, font=MONO)
# rx -> crossing -> out_pe
arrow([(1825, 1218), (1825, 1165)], color=RX, sw=2.4)
arrow([(2000, 1045), (2000, 470)], color=RX, sw=2.4)
text(1994, 760, "bytes, PIDs", size=10.5, fill=RX, font=MONO, anchor="end")

# pad logic <-> pins (the wires)
for y_pad, y_pin, lab in [(1300, 245, "D+"), (1330, 405, "D−")]:
    pass
arrow([(1380, 1485), (1380, 1519), (2052, 1519), (2052, 245), (2100, 245)], color=FG, sw=3, both=True)
arrow([(1430, 1485), (1430, 1511), (2044, 1511), (2044, 405), (2100, 405)], color=FG, sw=3, both=True)
arrow([(1560, 1485), (1560, 1503), (2036, 1503), (2036, 560), (2100, 560)], color=FG, sw=1.8)
for _x, _l in [(1386, "D+"), (1436, "D−"), (1566, "pull-up")]:
    text(_x, 1500, _l, size=10.5, fill=FG, font=MONO, weight=700)
# pins -> plug -> host
arrow([(2247, 670), (2247, 690)], color=FG, sw=2.4, both=True)
arrow([(2247, 810), (2247, 830)], color=FG, sw=2.4, both=True)

a('</svg>')
dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "Qomu USB Serial Map.svg")
open(dest, "w").write("\n".join(out))
print("ok")
