#!/bin/bash
# Build the Qomu usb2serial gateware with the open toolchain (ql_symbiflow).
# Run inside the symbiflow-qomu container (see docker/Dockerfile), from the Mac:
#
#   docker run --rm --platform linux/amd64 -v ~/src/qorc-sdk:/qorc-sdk symbiflow-qomu \
#       bash /qorc-sdk/qomu_apps/qomu_scheme/gateware/usb2serial/build.sh [check|compile|all]
#
#   check   - elaborate the design with iverilog against the toolchain's cell
#             models (seconds; catches syntax / port mistakes)
#   compile - full synthesis, place and route, bitstream and C header
#   install - turn the generated header into ../../fpga/qomu_fpga.c (the array
#             main.c loads); the SpDE original is kept as qomu_fpga.c.spde
#   all     - check + compile + install (default)
#   sim     - run the USB enumeration testbench (sim/usb_enum_tb.v) on the
#             RTL with iverilog; add VCD=1 to also write a waveform
set -e
STEP=${1:-all}
PKG=${PKG:-WR42}            # Qomu: EOS S3 in the WLCSP package
TOP=AL4S3B_FPGA_top
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$STEP" != install ]; then     # install needs no toolchain
    source "$INSTALL_DIR/conda/etc/profile.d/conda.sh"
    conda activate
fi

SRCS="AL4S3B_FPGA_Top.v AL4S3B_FPGA_IP.v AL4S3B_FPGA_Registers.v AL4S3B_FPGA_QL_Reserved.v
      FPGA_FIFO.v f512x8_512x8.v r512x8_512x8.v
      usb2m4_serial.v usb_serial_ctrl_ep.v usb_m4_bridge_ep.v
      usb_fs_pe.v usb_fs_in_pe.v usb_fs_out_pe.v usb_fs_in_arb.v usb_fs_out_arb.v
      usb_fs_rx.v usb_fs_tx.v usb_fs_tx_mux.v
      edge_detect.v strobe.v strobe_out4.v"

cd "$HERE/rtl"

if [ "$STEP" = check ] || [ "$STEP" = all ]; then
    CELLS=$(find "$INSTALL_DIR" -name 'pp3_cells_sim.v' 2>/dev/null | head -1)
    echo "== check: iverilog elaboration (cell models: ${CELLS:-not found})"
    if [ -n "$CELLS" ]; then
        iverilog -g2005 -I "$(dirname "$CELLS")" -s $TOP -o /tmp/usb2serial_check.vvp "$CELLS" ../sim/check_models.v $SRCS
        echo "== check: OK"
    else
        echo "== check: pp3_cells_sim.v not found, skipping"
    fi
fi

if [ "$STEP" = sim ]; then
    CELLS=$(find "$INSTALL_DIR" -name 'pp3_cells_sim.v' 2>/dev/null | head -1)
    cp "$CELLS" "$HERE/sim/pp3_cells_sim.ref.v"   # reference copy (git-ignored)
    echo "== sim: USB enumeration testbench"
    iverilog -g2005 -I "$(dirname "$CELLS")" -s top_tb -o /tmp/usb_enum_tb.vvp \
             "$CELLS" ../sim/check_models.v $SRCS ../sim/usb_enum_tb.v
    ( cd "$HERE/sim" && vvp -n /tmp/usb_enum_tb.vvp ${VCD:++vcd} ) 2>&1 | tee "$HERE/sim.log"
    # count testbench assertion failures only (not the RAM models' optional init.mem notes)
    N=$(grep -c "ERROR (" "$HERE/sim.log" || true)
    echo "== sim: $N testbench failures (log: sim.log)"
fi

if [ "$STEP" = compile ] || [ "$STEP" = all ]; then
    echo "== compile: ql_symbiflow, package $PKG (log: $HERE/build.log)"
    # timing constraints: use ql_symbiflow's -s if it has one, otherwise
    # drop them into the per-build SDC file it generates and re-run P&R
    SDCOPT=""
    if ql_symbiflow -h 2>&1 | grep -q -- '-s '; then SDCOPT="-s ../qomu.sdc"; fi
    echo "== compile: constraints via ${SDCOPT:-generated-file fallback}"
    mkdir -p build
    touch build/.compile_start
    # fallback: seed the generated SDC file so the first pass already uses the
    # current constraints (ql_symbiflow keeps an existing file)
    [ -n "$SDCOPT" ] || cp ../qomu.sdc build/${TOP}_dummy.sdc
    ql_symbiflow -compile -d ql-eos-s3 -P "$PKG" -v $SRCS -t $TOP \
                 -p ../qomu.pcf $SDCOPT -dump header 2>&1 | tee "$HERE/build.log"
    if [ -z "$SDCOPT" ] && ! cmp -s ../qomu.sdc build/${TOP}_dummy.sdc; then
        echo "== compile: constraints file was replaced; re-running pack/place/route"
        cp ../qomu.sdc build/${TOP}_dummy.sdc
        rm -f build/${TOP}.net build/${TOP}.place build/${TOP}.route build/${TOP}.fasm build/${TOP}.bit
        make -f Makefile.symbiflow build/${TOP}.bit 2>&1 | tee -a "$HERE/build.log"
        ( cd build && symbiflow_write_bitheader -b ${TOP}.bit ) 2>&1 | tee -a "$HERE/build.log"
        cp build/${TOP}_bit.h . 2>/dev/null || true
    fi
    # never let a failed compile hand an old bitstream to install
    if [ ! build/${TOP}.bit -nt build/.compile_start ] || [ ! ${TOP}_bit.h -nt build/.compile_start ]; then
        echo "== compile: FAILED, no new bitstream (see $HERE/build.log); not installing"
        exit 1
    fi
    echo "== timing (route.log):"
    grep -A12 "Final intra-domain critical path delays" build/route.log 2>/dev/null | head -14 || true
    echo "== outputs:"
    find . -newer ../qomu.pcf \( -name '*.h' -o -name '*.bin' -o -name '*.bit' \) | head
fi

if [ "$STEP" = install ] || [ "$STEP" = all ]; then
    HDR="$HERE/rtl/${TOP}_bit.h"
    OUT="$HERE/../../fpga/qomu_fpga.c"
    [ -f "$HDR" ] || { echo "== install: $HDR not found, run compile first"; exit 1; }
    [ -f "$OUT.spde" ] || cp "$OUT" "$OUT.spde"      # keep the SpDE original once
    {
        echo "// Generated by qomu_scheme/gateware/usb2serial/build.sh (open toolchain, ql_symbiflow)"
        echo "// on $(date -u '+%Y-%m-%d %H:%M UTC'). SpDE original: qomu_fpga.c.spde"
        echo "#include <stdint.h>"
        echo
        # const: stays in the image, not copied to RAM (see main.c)
        sed -e 's/^uint32_t[[:space:]]*axFPGABitStream\[\]/const uint32_t\taxFPGABitStream[]/' \
            -e '/axFPGAMemInit/,$d' "$HDR"
        echo "uint32_t  axFPGABitStream_length = sizeof(axFPGABitStream);"
    } > "$OUT"
    echo "== install: wrote $(basename "$OUT") ($(grep -o '0x' "$OUT" | wc -l) words)"

    # Pad (IOMUX) setup the bitstream expects, as a C function main.c calls
    # before load_fpga() (see iomux2c.py for the details)
    JLK="$HERE/rtl/build/${TOP}_iomux.jlink"
    IOC="$HERE/../../fpga/qomu_fpga_iomux.c"
    [ -f "$JLK" ] || { echo "== install: $JLK not found"; exit 1; }
    python3 "$HERE/iomux2c.py" "$JLK" "$IOC"
fi
