# QuickLogic open FPGA toolchain (ql_symbiflow v1.3.0) for EOS S3, fixed so it
# still installs today. Build on the Mac (Apple Silicon needs amd64 emulation):
#
#   docker build --platform linux/amd64 -t symbiflow-qomu \
#       ~/src/qorc-sdk/qomu_apps/qomu_scheme/gateware/docker
#
# Same packages and pins as the official v1.3.0 installer
# (conda_build_install_package.sh), with two fixes:
#   - Miniconda with Python 3.7. The installer fetches the latest Miniconda,
#     whose Python can't be downgraded to the 3.7 these packages were built for.
#   - Every step must succeed. The installer has no error checking, so it used
#     to finish "successfully" without yosys, vpr or iverilog.
# gtkwave (GUI) is left out.
FROM ubuntu:20.04

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=America/Denver
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git make xz-utils \
    && rm -rf /var/lib/apt/lists/*

ENV INSTALL_DIR=/opt/symbiflow/eos-s3
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN mkdir -p $INSTALL_DIR \
 && wget -q https://repo.anaconda.com/miniconda/Miniconda3-py37_4.12.0-Linux-x86_64.sh -O /tmp/mc.sh \
 && bash /tmp/mc.sh -b -p $INSTALL_DIR/conda \
 && rm /tmp/mc.sh

RUN curl -fsSL https://storage.googleapis.com/symbiflow-arch-defs-install/quicklogic-arch-defs-a409c551.tar.gz \
      | tar -C $INSTALL_DIR -xz

RUN source $INSTALL_DIR/conda/etc/profile.d/conda.sh \
 && F="-y --override-channels -c defaults -c conda-forge" \
 && conda install $F -c quicklogic-corp/label/ql yosys="0.8.0_0002_gc3b38fdc 20200901_073908" \
 && conda install $F -c quicklogic-corp/label/ql yosys-plugins="1.2.0_0009_g9ab211c 20201001_121833" \
 && conda install $F -c quicklogic-corp/label/ql vtr="v8.0.0_rc2_2894_gdadca7ecf 20201008_140004" \
 && conda install $F -c quicklogic-corp iverilog \
 && conda install $F make lxml simplejson intervaltree git pip \
 && conda activate \
 && pip install python-constraint serial git+https://github.com/QuickLogic-Corp/quicklogic-fasm@318abca \
 && conda clean -afy

ENV PATH="${INSTALL_DIR}/quicklogic-arch-defs/bin:${INSTALL_DIR}/quicklogic-arch-defs/bin/python:${PATH}"

# VPR (vtr package) links against Intel TBB 2020 (libtbb.so.2), which the
# conda package doesn't bring along; Ubuntu 20.04 ships it as libtbb2.
RUN apt-get update && apt-get install -y --no-install-recommends libtbb2 \
    && rm -rf /var/lib/apt/lists/*

# fail the build here, not later, if anything is still missing
RUN source $INSTALL_DIR/conda/etc/profile.d/conda.sh && conda activate \
 && which yosys vpr genfasm iverilog ql_symbiflow \
 && for t in yosys vpr genfasm; do \
        if ldd "$(which $t)" | grep "not found"; then echo "$t: missing libraries"; exit 1; fi; \
    done
