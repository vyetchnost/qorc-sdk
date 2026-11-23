# usb2serial gateware for Qomu, open-toolchain port

QuickLogic's USB-serial FPGA design (`s3-gateware/usb2serial`, built originally
with the proprietary SpDE tool), ported to the open `ql_symbiflow` toolchain so
we can change it. Goal: add RX flow control (NAK when the USB-to-M4 FIFO is
full) so bulk pastes into the Scheme REPL are never lost.

Changes from the s3-gateware RTL (commit 2f8fed6):

- `AL4S3B_FPGA_Top.v`: only the USB pins are top-level ports; 48 MHz clock
  straight from Sys_Clk1 (the /1.5 divider and clock switch are removed; the
  firmware always uses clock_select = 0).
- `AL4S3B_FPGA_IP.v`: SpDE `bipad` cells replaced by tri-state assigns.
- `r512x8_512x8.v`, `f512x8_512x8.v`: rewritten on the toolchain's
  `RAM_8K_BLK` / `FIFO_8K_BLK` (were SpDE/QuickWorks wrappers).
- `usb_fs_tx_mux.v`: recreated from the TinyFPGA USB core (missing upstream).
- `strobe_out4.v`: duplicate `dflip` removed.
- `qomu.pcf`: Qomu pins, from the schematic: D+ FBIO_31/E4, D- FBIO_28/F3,
  pull-up control FBIO_23/E2.

Build: see the header of `build.sh` (runs in the `symbiflow-qomu` container built from `../docker/Dockerfile`).
