# Doom port: technical notes

How the port is put together and what the hardware taught us on the way. General hardware facts that came out of this work live in the hardware and firmware guide,
[`../docs/hardware-and-firmware-guide.md`](../docs/hardware-and-firmware-guide.md); this page is about Doom itself. (Tags: **[tested]** observed on the
camera, **[disasm]** read from the disassembly, **[inferred]** reasoned from evidence.)

## Build and runtime assets
- **[tested]** Base is doomgeneric commit `e257a09`, newlib 4.4.0.20231231, shareware `DOOM1.WAD` 1.9. The WAD stays in the SD card root even though Doom's code is in
  flash. `build.sh` builds the relocatable `DOOM.GAME.BIN`; `build_flash.sh` builds `DOOM.FLASH.BIN` (the image the patcher places at flash `0x2A0000`) from an emulator
  profile and a generated split linker script.
- **[disasm]** `mksplit.py` keeps the renderer, game loop, platform layer and tables whole in RAM. Other code goes to flash unless its share of the profiled frame
  instructions exceeds `1e-5`; non-core read-only data also goes to flash. That avoids leaving unexercised doors, lifts or monsters in slow flash just because the
  profiling demo never reached them.
- **[tested]** The Games button opens Doom directly. The loading title is decoded from the WAD before Doom opens it; do not open the same WAD twice concurrently (that crashed
  the file layer).

## Memory layout
| Component | Arrangement |
|---|---|
| Cold code and read-only data | run in place from flash `0x062A0000`; the build requires the end to be <= `0x063F0000` |
| Hot code, core read-only data, data, BSS | fixed RAM at `0x02023000`, capacity `0x74000`; the unused tail becomes zone memory |
| Large renderer arrays | `0x020F5000`, capacity `0x14000`: `openings`, `viewangletox`, `drawsegs`, `zlight`; the unused tail becomes zone memory |
| Main zone | the largest firmware heap block, less 24 KiB reserved for the firmware, page-rounded (about 716 KiB plus the fixed-area tails) |
| Newlib arena | 96 KiB; about 64 KB used after start-up |
| SD bounce buffer | 16 KiB from the firmware heap, 64-byte aligned |
| Stack | 32 KiB from the firmware heap, switched to by `hx_stack.S` |

- **[tested]** The first, relocatable layout used 464 KiB at `0x02023000`, 80 KiB at `0x020F5000` and its dead relocation table as zone. Even with separate rovers for small
  allocations it failed after about 93 seconds with 228,904 free bytes but a largest free block of only 34,928 bytes. Fragmentation of the zone was the problem, which is
  what motivated running Doom from flash and reclaiming most of the firmware heap.
- **[tested]** `Z_AddRegion` adds independent zone extents with permanent sentinels, so blocks never merge across gaps. Allocations up to 1 KiB plus the block header use a
  separate rover, and sound lumps are purgeable. Level-tagged memory is freed when leaving a level for a demo page or intermission.

## Why the code runs from flash (and how it was split)
- **The first build** was a single relocatable image (`DOOM.GAME.BIN`): code about 359 KB (-O2) or 322 KB (-Os), read-only data 99 KB, data 60 KB, bss 241 KB (visplanes 85 KB, openings 40 KB and so on), and over ten
  thousand relocations. Together with Doom's zone memory, that did not fit the roughly 889 KiB of heap free in a game window. Running the code from flash instead saves about 420 KB of RAM.
- **Flash speed:** code that fits the instruction cache runs at the same speed from flash as from RAM, but about 100 KB of straight-line code took 55 s from flash against 3 s from RAM (**18x slower**; cache line fills
  arrive at about 3.6 MB/s), and data reads from flash are 8x slower sequentially and 17x slower at one word per cache line. So only cold code and cold read-only data belong in flash.
- **Profile:** an emulator run over about 530 demo frames (the title and the first level's demo) showed start-up costing about 20 M instructions and demo frames 1.3-2.3 M instructions each. Of about 260 KB of code,
  103 KB executed during frames; 99% of frame time was in 29 KB and 99.9% in 61 KB (`R_DrawColumn` 30%, `R_DrawSpan` 24%, `V_DrawPatch` 10%, `R_RenderSegLoop` 5%). 142 KB never ran in the demo. That profile is what
  `mksplit.py` uses to decide what goes in RAM and what stays in flash.

## Camera settings while playing
As a precaution, turn off auto power-off and the screensaver in the camera's Settings before playing. Doom runs its own loop and reads the key device itself, so the camera's idle timers (the screensaver, and auto power-off
which shows "bye bye") may not be reset by key presses; whether they still expire during normal play has not been measured.

## Video, files, errors
- **[tested]** Doom renders 320x200 straight into display-layer rows 20-219 (a 200 -> 240 stretch is applied in place), avoiding a 64,000-byte `I_VideoBuffer` zone allocation.
  Screen melt is disabled, saving two more 64,000-byte transition buffers. Palette entry 0 is mapped to RGB565 value 1, because pure black appeared as the display's
  transparent colour (magenta bands).
- **[tested]** Newlib file calls wrap the firmware's file descriptors (offset by 3 for stdin/stdout/stderr). All reads use the sector-aligned bounce-buffer pattern described in
  the hardware and firmware guide (reads that start mid-sector are corrupted by the SD DMA and cache invalidation).
- **[tested]** `_exit` and Doom fatal errors show a solid red screen and loop while kicking the watchdog. Quit returns through the host, which powers the camera off. A red
  loading bar is drawn at the top of the screen while a level loads.
- **[tested]** Save games are written to the card root as `doomsav0-7.dsg`.

## Controls
Up/down walk while held, left/right turn, the shutter fires. A power tap changes weapon; holding power (for about half a second to a second, as decided by the key driver) is Escape, the menu. Holding it longer does
nothing more: Quit in Doom's menu is what powers the camera off. Use (doors, switches) is automatic: if you are walking forward and stop making progress for about 150 ms, the port presses Use once ("bump to use"). In menus the
shutter is Enter and up/down move the cursor. The camera reports one key at a time, so there are no key combinations.

## Sound and music
- **[tested]** Sound effects: Doom's unsigned 8-bit 11025 Hz lumps are converted to signed 16-bit and mixed in software (up to Doom's 8 channels) into a ring of small PCM16
  buffers played through the firmware's audio DMA; the next buffer is queued from the audio callback on event bit 2 (see `hx_sound.c`). Music is a software synth for Doom's
  MUS format (`hx_music.c`). Details of the audio engine: [`../docs/analysis/sound.md`](../docs/analysis/sound.md), [`MUSIC.md`](MUSIC.md) (the music options we weighed) and [`../docs/analysis/mp3-decoder.md`](../docs/analysis/mp3-decoder.md).

## Performance
- **[tested]** The release build runs at roughly 26-32 fps, judged by the worst frame rather than the average. The firmware's own main loop costs about 38 ms per frame, so
  Doom runs its own loop from the game host (keys come from the key device, not the event queue). Code in flash is only fast while it fits the instruction cache (about 18x slower
  otherwise), which is why hot code is copied to fixed RAM.

## What the crashes taught (and why they matter to any program)
- **[tested]** The firmware's file-system buffer at `0x0201B000` corrupted the first zone; a DMA-written region up to `0x02022FFF` corrupted later guards; the display layer's
  76,800-byte screen copies overwrote a statically placed main-RAM stack (so the stack now comes from the firmware heap); unaligned file reads corrupted patch data; and the
  firmware's event-loop overhead halved the frame rate. These were platform issues, not doomgeneric logic failures.
- **[tested]** A useful memory test writes address-encoded patterns, writes them back, leaves a dirty tail, runs the real workload over many frames, and checks for old, moved or
  foreign data. Quiet fingerprint scans were not enough: DMA may rewrite identical values, and the display copies only become active once there is full-screen content.
- **[inferred]** If you use borrowed static RAM, a clean power-off and reboot is the safe way back to the stock camera functions.
