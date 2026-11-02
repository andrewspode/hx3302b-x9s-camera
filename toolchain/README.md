# Toolchain: a C compiler for this CPU

The camera's CPU is OpenRISC 1000 with two deviations from every standard toolchain: it is **little-endian** (standard OpenRISC is big-endian) and it has **no branch delay slots** (a `l.jal`'s next
instruction runs after the call returns, so `r9` = call + 4). These scripts turn stock binutils and GCC into a private cross compiler for it, installed with the prefix `or1kle-elf-` so it can't be mistaken for a
standard OpenRISC toolchain.

## Why we built our own
| Option | Little-endian | No delay slots | Notes |
|---|---|---|---|
| Distro `gcc-or1k-elf` 11.2 / `binutils-or1k-elf` 2.38 | no | no | upstream port |
| Upstream GCC master (`gcc/config/or1k/or1k.opt`) | no `-mlittle-endian` | no delay-slot option | |
| Binutils (`cpu/or1k.cpu`) | ? | **yes**: machine `or1knd` ("no branch delay slot") | |
| Samuel Holland's 2022 patches "or1k: Add support for a little-endian target variant" | **yes** | no | not in GCC master |
| Byte-swapping a big-endian build | no | | breaks any code touching bytes inside words; only OK for hand-checked assembly |

None gives both at once, so `patch-sources.sh` applies a small set of edits to binutils 2.43.1 and GCC 14.2.0 that make the OpenRISC target little-endian and delay-slot-free everywhere, with no options. Each edit checks that
the original text is present first, so a wrong source version fails loudly.

## Building it
```sh
toolchain/setup.sh [PREFIX]     # default PREFIX = out/toolchain
```
That downloads binutils 2.43.1, GCC 14.2.0 and newlib 4.4.0.20231231 (each checked against a fixed sha256), patches binutils and GCC with `patch-sources.sh`, and builds and installs them. It needs a Linux build
environment (curl, tar, xz, python3, make, a C compiler), no root, about 6 GB of disk while building and 30-60 minutes. The build scripts in `doom/` and `sdk/` run it for you if the compiler is missing; set `OR1KLE` to use a
compiler installed somewhere else. The pieces it runs, if you want them separately: `patch-sources.sh SRC_DIR`, `build.sh SRC_DIR PREFIX` (binutils and GCC) and `build-newlib.sh NEWLIB_SRC PREFIX`.

Compile with: `or1kle-elf-gcc -O2 -fno-pic -mhard-div -mhard-mul -ffreestanding -nostdlib`. The SDK (`../sdk/`) supplies the start-up code, linker script and system calls; newlib is built without
libgloss because the system calls come from the SDK.

## Checked how
On a test function (loop, call, byte and word data, divide) the ELF says little-endian, a `.data` word `0x11223344` is stored `44 33 22 11`, and Ghidra (`OpenRISC:LE:32`) decodes every instruction with control flow
that is only correct without delay slots (for example, the instruction after a taken `l.bf` belongs to the fall-through path). The whole firmware port (Doom included) has since run on the camera.

## Known issues
- `or1kle-elf-objdump -d` mis-decodes instructions whose operand fields are non-zero (the opcode is right, the fields wrong: the CGEN field extraction still assumes big-endian somewhere). **Use Ghidra to
  disassemble**.
- PIC (`set_got`) and libgcc's soft-divide helpers (`lib1funcs.S`) still assume delay slots: **do not use `-fpic` or `-msoft-div`**.
- C only, no C++.

Licence: GPL-3.0-or-later (see `LICENSE`); the scripts patch GPL software.
