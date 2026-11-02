#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Andrew Spode
# Build the private little-endian, no-delay-slot OpenRISC toolchain (see patch-sources.sh).
# Usage: build.sh SRC_DIR PREFIX    e.g. build.sh ~/tools/or1kle-src ~/tools/or1kle
# Each step stops the script on failure (no && chains: set -e does not apply inside them).
# Needs no root: GCC's prerequisites (gmp, mpfr, mpc, isl) are downloaded into the source tree; docs are skipped (no makeinfo).
set -eu
S=$1; P=$2; J=$(nproc)
export MAKEINFO=true
mkdir -p $S/build-binutils $S/build-gcc $P
cd $S/build-binutils
../binutils-2.43.1/configure --target=or1k-elf --program-prefix=or1kle-elf- --prefix=$P --disable-nls --disable-werror \
  --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --disable-libctf
make -j$J MAKEINFO=true
make install MAKEINFO=true
cd $S/gcc-14.2.0
[ -e gmp ] || ./contrib/download_prerequisites
cd $S/build-gcc
PATH=$P/bin:$PATH ../gcc-14.2.0/configure --target=or1k-elf --program-prefix=or1kle-elf- --prefix=$P --disable-nls \
  --enable-languages=c --without-headers --with-newlib --disable-shared --disable-threads --disable-libssp \
  --disable-libquadmath --disable-libgomp --disable-libatomic --disable-multilib \
  AS_FOR_TARGET=$P/bin/or1kle-elf-as LD_FOR_TARGET=$P/bin/or1kle-elf-ld \
  AR_FOR_TARGET=$P/bin/or1kle-elf-ar RANLIB_FOR_TARGET=$P/bin/or1kle-elf-ranlib
PATH=$P/bin:$PATH make -j$J MAKEINFO=true all-gcc all-target-libgcc
PATH=$P/bin:$PATH make MAKEINFO=true install-gcc install-target-libgcc
echo "toolchain installed in $P"
