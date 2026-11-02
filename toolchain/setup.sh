#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Andrew Spode
# One command from nothing to a working compiler: downloads binutils 2.43.1, GCC 14.2.0 and newlib 4.4.0.20231231 (checked against
# fixed sha256 sums), patches binutils and GCC (patch-sources.sh), builds and installs everything into PREFIX.
# Usage: setup.sh [PREFIX]    default PREFIX = <repo>/out/toolchain; sources and build folders go to <repo>/out/deps/toolchain-src
# Needs: sh, curl, tar, xz, python3, make, a C compiler and the usual build tools; about 6 GB of disk while building and 30-60 minutes.
# Safe to run again: finished steps are skipped (a finished build leaves PREFIX/bin/or1kle-elf-gcc and PREFIX/or1k-elf/lib/libc.a).
set -eu
R=$(cd "$(dirname "$0")/.." && pwd); P=${1:-$R/out/toolchain}; W=$R/out/deps/toolchain-src; H=$R/toolchain
mkdir -p "$W" "$P"
get() { # name, url, sha256
  if [ ! -f "$W/$1" ]; then curl -fL --retry 3 -o "$W/$1.part" "$2"; mv "$W/$1.part" "$W/$1"; fi
  echo "$3  $W/$1" | sha256sum -c --quiet - || { echo "setup: checksum of $1 does not match" >&2; exit 1; }
}
get binutils-2.43.1.tar.xz https://ftp.gnu.org/gnu/binutils/binutils-2.43.1.tar.xz 13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd
get gcc-14.2.0.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
get newlib-4.4.0.20231231.tar.gz https://sourceware.org/pub/newlib/newlib-4.4.0.20231231.tar.gz 0c166a39e1bf0951dfafcd68949fe0e4b6d3658081d6282f39aeefc6310f2f13
if [ ! -x "$P/bin/or1kle-elf-gcc" ]; then
  [ -d "$W/binutils-2.43.1" ] || tar -C "$W" -xf "$W/binutils-2.43.1.tar.xz"
  [ -d "$W/gcc-14.2.0" ] || tar -C "$W" -xf "$W/gcc-14.2.0.tar.xz"
  [ -f "$W/.patched" ] || { sh "$H/patch-sources.sh" "$W"; touch "$W/.patched"; }
  sh "$H/build.sh" "$W" "$P"
fi
if [ ! -f "$P/or1k-elf/lib/libc.a" ]; then
  [ -d "$W/newlib-4.4.0.20231231" ] || tar -C "$W" -xf "$W/newlib-4.4.0.20231231.tar.gz"
  NANO=1 sh "$H/build-newlib.sh" "$W/newlib-4.4.0.20231231" "$P"
fi
echo "toolchain ready in $P (build folders in $W can be deleted)"
