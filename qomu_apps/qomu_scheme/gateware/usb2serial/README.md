# usb2serial gateware for Qomu, open-toolchain port

QuickLogic's USB-serial FPGA design (`s3-gateware/usb2serial`, built originally
with the proprietary SpDE tool), ported to the open `ql_symbiflow` toolchain so
we can change it. It now has RX flow control (NAK while the USB-to-M4 FIFO
is full), so bulk pastes into the Scheme REPL are never lost.

Changes from the s3-gateware RTL (commit 2f8fed6):

- `AL4S3B_FPGA_Top.v`: only the USB pins are top-level ports; 48 MHz clock
  straight from Sys_Clk1 (the /1.5 divider and clock switch are removed; the
  firmware always uses clock_select = 0).
- `AL4S3B_FPGA_IP.v`: SpDE `bipad` cells replaced by tri-state assigns.
- `r512x8_512x8.v`: rewritten on the toolchain's `RAM_8K_BLK` (was a
  SpDE/QuickWorks wrapper).
- `f512x8_512x8.v`: first-word-fall-through FIFO built from `r512x8_512x8`
  plus fabric pointers and the S3 flag encoding. The `FIFO_8K_BLK` hard FIFO,
  as mapped by the toolchain, did not move data on the board; that version is
  kept behind `USE_HARD_FIFO`.
- `usb_fs_tx_mux.v`: recreated from the TinyFPGA USB core (missing upstream).
- `strobe_out4.v`: duplicate `dflip` removed.
- `usb_fs_rx.v`: `dvalid_tkn` / `dvalid_data` assigned with `<=` instead of `=`
  inside clocked blocks (an upstream race that made simulation clock each bit twice).
- `r512x8_512x8.v`: uses the native 1024x8 shape of the 8 Kbit block.
- `usb_fs_rx.v`: line-state recovery restructured (pair + transition flag)
  and the bit-stuff check registered, so the single-cycle 48 MHz paths are
  shallow; packet-start resets driven from a registered copy (fan-out off
  the single-cycle path); behaviour unchanged.
- `usb_fs_tx.v`, `usb_fs_pe.v`: the transmitter runs on the 12 MHz clock, one
  bit per clock (`USB_TX_12MHZ`). At 48 MHz its single-cycle paths needed
  ~50 ns, and the board would not enumerate. Byte loads take priority over the
  bit shift, and the first CRC byte includes the last data bit.
- `AL4S3B_FPGA_Registers.v`: reading RDATA now pops the u2m FIFO (upstream
  compared the word address with the byte address 0x044 and never matched).
  Read-only debug registers: 0x14/0x18 USB counters (line transitions, packet
  starts, valid packets, transmitted packets), 0x1C/0x20 FIFO byte counters
  (u2m pushes, pops, pushes refused when full, m2u pushes).
- `usb_m4_bridge_ep.v`: RX flow control. A byte is pulled from the OUT
  endpoint buffer only when the u2m FIFO has room for 4; otherwise draining
  pauses and `usb_fs_out_pe` NAKs the next OUT until the M4 has read enough.
- `usb_fs_rx.v`, `usb_fs_pe.v`, `AL4S3B_FPGA_IP.v`: debug counters; the line
  transitions flip a bit at 48 MHz that is counted in the 12 MHz domain.
- `strobe.v`, `strobe_out4.v`: the clock-crossing strobe XORs two synchronised
  flops. Upstream used the first (possibly metastable) stage, so an event
  could arrive twice or not at all depending on clock phase and placement.
- `usb_fs_tx.v`: `USB_TX_FAST_START` starts the sync pattern on the clock after
  `pkt_start` instead of idling 7 bit times, and holds the line state at J
  while the output is off (upstream relied on the idle time to reach J). The
  reply turnaround drops from about 14.5 to about 8 bit times; the host gives up
  after 16-18.
- `usb_fs_pe.v`: debug counters (registers 0x14/0x18, 16 bit, wrapping): data
  packets sent, packets received with a bad PID/CRC, valid packets received,
  ACKs received.
- `qomu.sdc`: 48 MHz / 12 MHz clock constraints for timing-driven place and route.
- `qomu.pcf`: Qomu pins, from the schematic: D+ FBIO_31/E4, D- FBIO_28/F3,
  pull-up control FBIO_23/E2.

Simulation (`build.sh sim`, `sim/usb_enum_tb.v`): enumeration, M4-to-USB and
USB-to-M4 data, flow control with 17 packets against a full FIFO, and the
reply turnaround (fails above 12 bit times).

## 72 MHz (parked, 26 Nov 2026)

Goal: M4 at 72 MHz (about 1.5x faster Scheme). All clocks divide from one
oscillator, so 72 MHz leaves no integer divider for a 48 MHz USB clock.

- QuickLogic divided 72 MHz by 1.5 in the fabric with a combinational loop,
  which the open toolchain rejects. A glitch-free XOR divider needs a
  falling-edge flop; Yosys builds that with a logic-cell inverter in the clock
  path (not the cell's QCKS polarity bit), so every second 48 MHz edge would
  arrive several ns late and the receiver's short periods would fail.
- Instead, `USB_CLK72` runs the receiver from C21 = 72 / 2 = 36 MHz with 3
  samples per bit (`usb_fs_rx.v`); firmware `SCHEME_CPU_MHZ 72` in
  `inc/s3x_pwrcfg.h` sets the oscillator, C10 and the power policies, and
  `main.c` sets C21 after the HAL (whose 48 MHz request rounds to 72).
- Results: simulation passes, timing has 6 ns of margin, the CPU measured
  72.15 MHz, and the board enumerated. Under traffic, output went missing;
  with the CPU at 36 MHz (same USB clocks) it still did, so the cause is in
  the 3x gateware path. That led to the strobe and turnaround fixes above,
  which are kept for 48 MHz. With them, a 72 MHz build stopped starting at all
  (no firmware LED), even with a bitstream that had booted earlier: start-up
  at 72 MHz looks marginal. Next step when resumed: flash one 72 MHz build and
  power-cycle it several times to see whether starts are intermittent.

Build: see the header of `build.sh` (runs in the `symbiflow-qomu` container built from `../docker/Dockerfile`).
