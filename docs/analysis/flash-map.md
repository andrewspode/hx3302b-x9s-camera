# Flash map and boot layout

The camera's flash is a 4 MiB SPI NOR chip. The CPU sees it at **`0x06000000`** (offset X at `0x06000000 + X`), and the big body of code runs in place from there.
The two regions that are copied into RAM at boot, and how to read call targets in a disassembler, are explained once in the
[hardware and firmware guide](../hardware-and-firmware-guide.md#boot-flow-and-address-model). Evidence that the base is `0x06000000`: string pointers resolve there and the update routine's strings and code line up.

Notable offsets: the camera's one JPEG carrying EXIF (`HX-Tech`) is at about `0xA9B20`; the update strings are at `0xC0D00`-`0xC0E60` and `start usb update` is at
`0xBD430`; the code area between `0x14000` and `0x7E000` has 50-97% of `l.jal` targets landing on function prologues in each 8 KiB block, which is how code was told from data;
a smooth lookup ramp sits at `0x7FFF0`; the settings block starts `E7 07` and holds string IDs of the form `0x81xxxxxx`; and the internal photo store starts with a FAT-style
header (`EB 76 90 'MSDOS5.0'`) at `0x291000`, with the first photo data from `0x293000`.

## Memory map (confirmed by dumping the camera's memory)
| Address | What | Loaded from |
|---|---|---|
| `0x00000000`-`0x0000FFFF` | 64 KiB SRAM: first stage (code+data) at 0, then runtime data/buffers (vars at `0x1068`, `0x1360`, `0x1CD0`; `0x10000`+ reads 0) | flash `0x200` onward (address = flash - `0x200`); `0x0`-`0xFFF` matches exactly, later parts are overwritten at run time |
| `0x02000000`-`0x020111FF` | RAM image (the "in-place" code + its data) | flash `0x2E00`-`0x13FFF` (header `0x24`/`0x28`) |
| `0x02011200`-`0x021095FF` | static buffers; while a game window was open only `0x02011000`, `0x02013000`-`0x02015FFF`, `0x02019000`-`0x0201AFFF`, the screen layers `0x02097000`-`0x020BDFFF`, `0x020CF000`-`0x020D0FFF` and `0x020E2000` changed; `0x0201B000`-`0x02096FFF` (496 KiB) and most of the rest stayed untouched; with file I/O (ramio) `0x0201B000`-`0x0201BFFF` is the FILE SYSTEM's buffer, and a pattern test (a Doom test run) showed `0x0201C000`-`0x02022FFF` is rewritten constantly (a DMA writing the same values: fingerprints cannot see it), so the usable block is `0x02023000`-`0x02096FFF` (464 KiB). LESSON: check borrowed RAM with a written pattern, not by watching for changes | |
| `0x02011200`-`0x021FFFFF` | RAM: bss, heap (free-list root `0x0201327C`), mode table `0x02019270`, capture buffers in the upper MiB. **Total RAM 2 MiB** (a read at `0x02200000` resets the camera; RAM size probe) | |
| `0x06000000`-`0x063FFFFF` | flash, memory-mapped; the big body runs here in place | |
The hidden mask boot ROM (which does the loading) is not visible in these windows; the firmware never calls it.

## Flash map summary
| Flash | What | Status |
|---|---|---|
| `0x00000`-`0x001FF` | header (`BLDR`, checksum, time, load fields; table below) | mostly decoded |
| `0x00200`-`0x02BFF` | first stage, loaded to SRAM 0 (vectors, SDRAM init, SPI flash driver, uart, memset...) | proven (memory dump) |
| `0x02C00`-`0x02DFF` | gap / padding | not looked at |
| `0x02E00`-`0x13FFF` | RAM image -> `0x02000000` (SD update, flash writer, helpers) | proven (memory dump) |
| `0x14000`-`0x7DFFF` | big body, runs in place: UI, modes, games (`games.md`), FatFs, JPEG, sensor, USB | code |
| `0x7E000`-`0xA5FFF` | pairs of u16 (e.g. `0x84D9 -> 0x52DD`): probably the FatFs GBK<->Unicode code-page tables | GUESS |
| `0xA6000`-`0xC7FFF` | rodata: strings, EXIF template, RIFF templates, game menu table | read |
| `0xC8000`-`0xCFFFF` | sensor/ISP tuning tables | read |
| `0xD0000`-`0xFFFFF` | **MP3 decoder code** (~260 functions; strings `[MP3 DEC] ...`). Not referenced by the body; its outgoing calls do not resolve at the flash address or at any single base tried, so it runs from somewhere else (an overlay) or is not plain OR1K throughout | OPEN |
| `0x100000`-`0x1005FF` | small block, looks like code; header `0x38` points here | OPEN |
| `0x100600`-`0x28FA6B` | resources (`resources.md`) | decoded |
| `0x290000`-`0x2901FF` | settings block | located, layout not decoded |
| `0x291000`-`0x3FFFFF` | internal photo store (photos taken without an SD card) | located, index not decoded |

## Header fields (512-byte sectors)
| Offset | Value | Meaning |
|---|---|---|
| `0x04` | `BLDR` | magic |
| `0x09` | `0x01` | version-block offset /16 |
| `0x10` / `0x14` | `0x315B3CE1` / `0x668258DC` | checksum (algorithm unknown, not checked by firmware or, apparently, the loader) / build time |
| `0x18` / `0x1C` | `0x803` / `0xC7B` | resource area: start `0x100600`, length `0x18F600` (ends `0x28FC00`) |
| `0x20` | `0x02000000` | RAM image load address |
| `0x24` / `0x28` | `0x17` / `0x89` | RAM image: flash `0x2E00`, length `0x11200` |
| `0x34` | `0x01234567` | constant |
| `0x38` | `0x800` | `0x100000` (the small block before the resource directory; not analysed) |
| `0x3C`, `0x40`, `0x44`, `0x48` | `0x0313C00F`, `1`, `0x00080010`, `0x203` | unknown (SPI/clock config?) |
| `0x4C`- | | code stub |
The first-stage load (flash `0x200` -> SRAM 0) is not described by an obvious field; Sprite_tm's trace of his camera read `0x2A00` bytes from `0x200`.
Note: disassemble the first stage with flash `0x200` at address 0, not flash offset 0.

## Evidence that the RAM image is a copy of flash
A dump of the camera's RAM shows that `0x02000000`-`0x0200FFFF` holds flash `0x2E00`-`0x12DFF` byte for byte (apart from 36 bytes of runtime state near `0x0200E6AC`).
The code that does the copy is not in our flash code, so it is the boot ROM (driven by the header) or the first-stage code. Icons are referenced by flash offset
(records `{offset, 32, 32}` in RAM), i.e. read through the SPI driver.

## First-stage region (`0x000`-`0x2C00`) and the loader question
Disassembled with the region mapped at address 0 (`tools/ghidra.sh`, base 0). What it contains:
- Exception-vector machinery: a routine that copies 256-byte handler templates to a vector page (`(addr>>8)<<8`) at `0x510`-`0x560`.
- **SDRAM-controller-style initialisation**: writes to SPRs `0x98C0`-`0x98CC` with timing constants, an init command sequence
  polling bit `0x40` of `0x98C4`, and a delay helper (`0x1E58`-`0x1F84`). So RAM is set up by flash code, not only by the ROM.
- The header stub (`0x4C`-`0x118`) ends by jumping to absolute `0x1E40`.
A scan of both this region and the in-place region (`0x2E00`-`0x14000`) for word-copy loops finds **none that populates a `0x0A...` destination**
(the only copy loops are the vector installer and a small 8-word copy at `0x7F9C`). So the RAM-resident region is either loaded by the
boot ROM from the header (not in the dump) or is a hardware alias of the flash. This remains open.

## Open
- The header fields at `0x18`-`0x2C` are decoded in the table above. Fields `0x3C`-`0x48` are still unknown.
- The checksum at `0x10`: not a plain 32-bit sum (LE or BE), xor, 16-bit sum, byte sum or CRC-32 of any prefix tried (starts 0, 0x18, 0x20, 0x38, 0x4C, 0x200, 0x2C00, 0x2E00; skipping 0x10, 0x10-0x17, or the whole 512-byte header). The firmware has no CRC or hash tables. The only code that reads the field is the SD updater.
- Whether a boot ROM verifies the header at power-up. (A changed-code update with the checksum left unchanged boots fine, so either it does not verify, or its check does not cover that area. The mask boot ROM itself has never been visible: the firmware does not call it, and `0x04000000` reads as zeros.)
