#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Andrew Spode
# Build newlib 4.4.0.20231231 (libc + libm; setup.sh sets NANO=1 for the small formatted I/O the Doom port needs to fit; no libgloss: system calls come from sdk/hxsdk) with the private or1kle toolchain.
# Usage: build-newlib.sh SRC_DIR PREFIX   e.g. build-newlib.sh out/deps/toolchain-src/newlib-4.4.0.20231231 out/toolchain (setup.sh does this)
# Installs into PREFIX/or1k-elf/{include,lib}. newlib's OR1K setjmp.S default path is safe without delay slots (checked).
set -eu
S=$1; P=$2; T=$P/bin
# libc/sys/or1k/mlock.c needs libgloss's or1k_critical_begin/end; our programs are single-threaded inside the game host: make the
# malloc lock a no-op (idempotent).
cat > "$S/newlib/libc/sys/or1k/mlock.c" <<'MLOCK'
/* hx330x: single-threaded (runs inside one firmware event handler); malloc lock is a no-op. Replaced by toolchain/build-newlib.sh. */
#include <reent.h>
void __malloc_lock(struct _reent *r) { (void)r; }
void __malloc_unlock(struct _reent *r) { (void)r; }
MLOCK
B="$S/../build-newlib${NANO:+-nano}"; mkdir -p "$B"; cd "$B"   # NANO=1: newlib nano formatted I/O (small printf/scanf, no float unless linked in)
export CC_FOR_TARGET=$T/or1kle-elf-gcc AS_FOR_TARGET=$T/or1kle-elf-as AR_FOR_TARGET=$T/or1kle-elf-ar RANLIB_FOR_TARGET=$T/or1kle-elf-ranlib \
       LD_FOR_TARGET=$T/or1kle-elf-ld NM_FOR_TARGET=$T/or1kle-elf-nm
export CFLAGS_FOR_TARGET="-O2 -fno-pic -mhard-div -mhard-mul -ffunction-sections -fdata-sections"
[ -f Makefile ] || "$S/configure" --target=or1k-elf --prefix="$P" --disable-multilib --disable-libgloss --disable-newlib-supplied-syscalls \
  --enable-newlib-reent-small --disable-newlib-fvwrite-in-streamio --enable-newlib-nano-malloc ${NANO:+--enable-newlib-nano-formatted-io} --disable-nls MAKEINFO=true
make -j"$(nproc)" MAKEINFO=true all-target-newlib
make MAKEINFO=true install-target-newlib
echo "newlib installed in $P/or1k-elf"
