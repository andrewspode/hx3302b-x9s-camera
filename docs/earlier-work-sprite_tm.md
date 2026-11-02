# Earlier work and credits: Sprite_tm's kid-cam firmware modding

**Jeroen Domburg (Sprite_tm)** published the earlier public work on this SoC family. Everything in this file comes from his
four-page article, in our own words. He should be credited in any write-up of this project. What we actually took from it is small and listed
at the end; the rest of this project came from our own dump.

- Article: https://spritesmods.com/?art=kid_cam (pages: intro, "Figuring out the ISA", "The modification", conclusion)
- Russian translation, Habr: https://habr.com/ru/companies/ruvds/articles/990774/
- Ghidra processor he wrote (OpenRISC little-endian): his `ghidra-or1k-le-hack`; maintained fork at
  https://github.com/aleph-void/ghidra-or1k-le-hack (its README says it was tested against his work, not a real HX330x image)

**Provenance warning.** These notes were gathered by reading the article through a summarising fetch tool, not by reading
every page ourselves. Re-read the original before quoting or publishing anything, and correct this file where it differs.

## His device vs ours

| | Sprite_tm's camera | This camera |
|---|---|---|
| Bought from | Taobao, a child's camera | not recorded |
| SoC | HX-Tech HX3302B ("HX330x family") | HX3302B |
| Flash | Spansion S25FL132K, 4 MiB SPI | JEDEC `C4 60 16`, 4 MiB SPI (marking `T25S32`) |
| SD update file | `hx330x_sdk.bin` | `Xly100s.bin` (found in our dump) |
| USB webcam mode | not mentioned | yes, UVC (`-UVC ON-` in strings) |

## What he established

- **Identifying the chip was the hard part.** He found almost nothing about the "HX3302B" or the company "HX-Tech" online
  (only AliExpress listings), and standard ISA detectors (cpu_rec, isadetect) did not work.
- **CPU: OpenRISC 1000 variant.** 32-bit, **little-endian** (OR1K is normally big-endian; he believes the SoC designers added a
  data-bus scrambler), and **no branch delay slots**. He edited an existing OR1K Ghidra definition to match, after which Ghidra
  decoded the whole binary.
- **Boot sequence, from a logic-analyser trace of the SPI flash:** the chip reads 512 bytes from address 0 (a header that
  lists what to load), then copies sections from flash into RAM, then fetches instructions directly from memory-mapped flash;
  once the instruction cache is on it switches to 16-byte cache-line fills. The SPI clock was about 70 MHz.
- **No integrity protection.** He reports no cryptographic hash and not even a CRC on the firmware, so modified images boot.
- **No usable debug UART.** A pad marked TX on his board was tied to the SD activity LED and carried no serial data.
- **Code layout quirk:** the firmware uses a scheduler/event system with function pointers stored in tables in flash, so
  Ghidra does not find a clean entry point; the tables' format and location had to be worked out by hand.
- **How he found things:** debug `printf` strings plus the shape of a standard printf implementation. For one key
  experiment he changed the JPEG-writing code to put an arbitrary block of memory into the EXIF data of a photo, then flashed and
  looked at the picture. That gave a way to read memory contents without a debugger.
- **The modification:** the current-mode variable is initialised to the main menu; changing that one value makes the camera
  boot straight into camera mode. He says finding the byte took far longer than the change.

## The SD-card update mechanism (his camera)

- Put a file named `hx330x_sdk.bin` in the SD card root.
- The camera checks that some header fields look valid, then reads a **timestamp field** in the file; if it is **newer
  than the one in flash**, the camera copies the whole file into flash.
- To build the file: dump the flash, **trim it so it contains only the firmware**, make the changes, save as the filename above.
  To get his image accepted he increased the timestamp past the firmware's.
- Before finding this he desoldered the flash and reprogrammed it with an external programmer.

## Things from the comments on his article (unverified)

- Another reader used the same board's screen and sensor with a different MCU, and saw the TX pad start to transmit at boot
  and then get shut down by the firmware.
- One comment gives limits for printing photos on a similar toy camera: 4:2:0 chroma subsampling, and a maximum file size
  per resolution (e.g. 3 MP at 210 KB). Not about this chip's firmware, but a hint about the JPEG encoder's constraints.

## How our camera differs, and what that means

Same SoC, very different firmware. From his article, his firmware and ours differ in nearly every specific we can compare: a different product, a
different SD update filename (`hx330x_sdk.bin` vs our `Xly100s.bin`), a different set of games (his has an unfinished or hidden one; ours has five
finished games and nothing hidden), a different update acceptance rule (his wants a *newer* timestamp; ours accepts any header time or checksum that
differs from the installed one), a different menu and window system, and our camera also has a UVC webcam / USB mass-storage mode that his article doesn't
discuss. None of his addresses, table locations or code offsets could be used, and we could not simply reuse his patches.

So his work gave us **broad clues, not specific ones**, and they were worth a lot:
- what the chip is, and that its CPU is a little-endian OpenRISC variant without branch delay slots (so Ghidra could be made to decode it);
- the boot shape: a 512-byte header, sections copied to RAM, the rest executed in place from memory-mapped flash;
- that there is no integrity protection worth the name, so modified images boot;
- that debug `printf` strings are the way into an unknown firmware, and that firmware is driven by function-pointer tables, not a clean entry point;
- that the SD card can update the flash from a file in its root, and that the file's header timestamp is what the check looks at;
- that putting memory into a photo's EXIF block is a way to read memory without a debugger.

Everything specific (what our file is called, where our tables are, what the update check compares, how to reach our own code) had to be found again in
our own dump.

## What we actually borrowed
- **The CPU identification** (little-endian OpenRISC 1000 variant, no branch delay slots).
- **The Ghidra processor definition.** We used a prebuilt OpenRISC little-endian / no-delay-slot extension ("Ghidra-Rare" v1.1.0, language
  `OpenRISC:LE:32:default`), which is based on `smaeul/ghidra-or1k` and his `ghidra-or1k-le-hack`. We installed it as is and did not rebuild it. Without it we
  would have had to write that processor module ourselves.
- **The EXIF-block memory dump trick**, which we reproduced to read RAM and low memory and so settle the address model.
- **A general expectation** that the SoC family has no real integrity checking and that the SD card can carry an update.

Everything else (the update filename and rule, the flash map, the window and mode system, the games, sound, display and input, and how to run our
own code) was worked out from our own dump and experiments.

## Related coverage
Other public material on the same SoC family, listed for reference (not something this project depends on): a Hackaday piece on a second HX3302B camera,
the G6 thumb camera (https://hackaday.com/2026/02/03/the-surprising-hackability-of-a-knock-off-chinese-toy-camera/), and an EXIF sample showing
`HX330X-V001.001` (https://gist.github.com/SuperIlu/eb003a17cfa4bd6881cdb239d2dfaf2e).
