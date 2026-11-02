# The update routine, from the disassembly

Notes on the code behind [how firmware updates work](../updating-the-firmware.md): the check at flash `0x4900`-`0x4D20` and the write loop at `0x51B4`-`0x52F4`. Read from a Ghidra disassembly
(`OpenRISC:LE:32:default`, flash at `0x06000000`) of your own dump; reproduce it with `tools/ghidra_scripts/DumpDisasm.java 06004900 06004d20` (the listings themselves are not published). The behaviour as a
whole is **proven on the camera**: a plain power-on with a changed-time `Xly100s.bin` flashes it, up to a full 4 MiB, and powers off with "bye bye". The status values, log strings and flash-driver
identification below are from the disassembly; the SPI opcodes that identify the driver functions are solid evidence.

## The check, step by step
1. Checks an SD-present status byte at `0x0201A011` (> 1). If not: logs `no sdc`, returns -1.
2. Logs `[SD UPDATE]: find update file<%s>` with the name **`Xly100s.bin`**, opens it (read mode). Fails: returns -1.
3. **Reads the installed firmware's header from flash:** offset 4 must be `BLDR`; the byte at offset 9 `<<4` is the version-block
   offset (here `0x01<<4 = 0x10`); there it reads *checksum* (`0x315B3CE1`) and *time* (`0x668258DC`, 2024-07-01 07:21:00 UTC).
4. **Validates the SD file's header the same way.**
5. Sets a status (`CheckNeedUpdata :%d`) and decides on its sign:

| status | meaning | result |
|---|---|---|
| 1 | installed flash has no `BLDR` header (blank/corrupt) | **updates** (recovery path) |
| 2 | installed version block is the ASCII placeholder `"0123"+"4567"` | **updates** |
| 3 | file's version-block offset differs from installed | **updates** |
| 4 | file's version block is the `0123`+`4567` placeholder | **updates** (force) |
| 5 | file's checksum or time differs from installed | **updates** |
| 0 | checksum and time both identical | refuses: `check same ,no to update` |
| -1 | read error on the file | refuses |
| -2 | file has no `BLDR` at offset 4 | refuses |
| -3 / -4 | short read of the offset byte / version block | refuses |

(The refusal path prints the same `check same ,no to update` text for every non-positive status.)

## The firmware's own debug strings label these fields
The routine's `printf` formats are `cur_checksum:%x,cur_time:%x` (installed, read from flash) and `bin_checksum:%x,bin_time:%x`
(the SD file), plus `mem:%x` / `bin_mem:%x` (the version-block offset) and `CheckNeedUpdata :%d` (the status). So:
- the dword at header `0x10` is a **checksum** (`0x315B3CE1` on this camera) and the dword at `0x14` is a **time** (`0x668258DC`);
- the decision compares checksum and time for *equality* only; this routine does not compute or verify the checksum;
- after the check passes, the code sets up a screen buffer (`sd_update w h:%d,%d`, `scan_mode`, `clustsize`, `[SD update] clbtal err` = the FAT
  cluster-link-map table for fast seeking, `[SD UPDATE] malloc buf err`) and continues into the write loop described below.
Address constants in the listing resolve to the strings quoted above.

## The flash driver (all in the flash image)
| Function | Does | Evidence |
|---|---|---|
| `0x06003BF0` | erase one 4 KiB sector (address masked to `& 0xFFFFF000`) | sends write-enable, then opcode `0x20` + 3 address bytes |
| `0x06003A7C` / `0x060039E8` | program, in 256-byte pages | opcode `0x02`, page loop compares to `0x100` |
| `0x06003E84` | read | opcode `0x03` + 3 address bytes |
| `0x06006DD8` | send one byte over SPI (all of the above use it) | called with each opcode/address byte |
| `0x06006FFC` | chip-select / write-enable control | called around each command |

So the flash is standard SPI NOR (consistent with `C4 60 16`, 4 MiB, 24-bit addressing).

## The write loop
For `off = 0, 0x1000, ... < file size` (`0x1C(0x0201A554)`):
1. read the next 4096 bytes of `Xly100s.bin` into a buffer (error code `0x52` = 'R');
2. **first pass, `off == 0` only: zero the first `0x200` bytes of the buffer** (the `BLDR` header; `0x04003C9C` is presumably `memset`);
3. erase the sector at flash `off` (error `0x45` = 'E'), program it (`0x57` = 'W'), read it back and compare (`0x56` = 'V');
4. update a progress percentage.
After the last chunk it sets `pass = 2`, rewinds the file, and the loop runs **once more for chunk 0 only**, now without the zeroing,
then prints `OK`. So:
- **File offset == flash offset**: the file is written from flash address 0 upward; it need only be as long as the part you change.
- **Sector 0 is erased and written first with a zeroed header, and rewritten with the real header last.** The header is the commit point.
  A power cut in between leaves a zeroed header (probably unbootable from flash; whether the boot ROM has an SD fallback is unknown).
- **Nothing in this loop checks the file's checksum field;** it writes whatever the file contains and verifies the flash against the file.
- **A trimmed file** therefore means: a prefix of the flash image (header included, since sector 0 is always rewritten), padded to a
  multiple of 4096 (the loop writes whole 4 KiB chunks; what fills the tail of a short last read is unknown).

## Still not known
- Whether the mask boot ROM verifies the header checksum at boot (the updater doesn't; changed code with the old checksum boots fine).
- What fills the tail of a short last read when the file is not a multiple of 4 KiB (pad with `0xFF` to be safe).
- Whether the boot ROM has an SD-card fallback if the header is left zeroed by an interrupted update.
