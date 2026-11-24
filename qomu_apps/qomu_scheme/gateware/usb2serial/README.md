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
- `qomu.sdc`: 48 MHz / 12 MHz clock constraints for timing-driven place and route.
- `qomu.pcf`: Qomu pins, from the schematic: D+ FBIO_31/E4, D- FBIO_28/F3,
  pull-up control FBIO_23/E2.

Simulation (`build.sh sim`, `sim/usb_enum_tb.v`): enumeration, M4-to-USB and
USB-to-M4 data, and flow control with 17 packets against a full FIFO.

Build: see the header of `build.sh` (runs in the `symbiflow-qomu` container built from `../docker/Dockerfile`).
