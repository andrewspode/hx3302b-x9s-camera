# Documentation

Notes on the X9S camera, its HX3302B SoC and its firmware, in a sensible reading order. Statements are tagged in the guide as read from the
disassembly or tested on the camera.

## The camera
- [**Specifications and hardware photos**](camera-specs.md): the printed manual's specification table, photos of the board and chips, and how the claims compare with the firmware.

## How the hardware and firmware work
- [**Hardware and firmware guide**](hardware-and-firmware-guide.md): the main technical guide: CPU and boot, memory and flash layout, callable firmware services, hardware rules,
  UI modes and events, updates and USB reading, resources, and the open questions. Start here, then use the pages below for the detail.
- [Flash map](analysis/flash-map.md): what lives where in the 4 MiB flash, the 512-byte header, and the in-place versus RAM-resident code.
- [Resources](analysis/resources.md): sounds, JPEG art, the font and the 18-language UI strings.
- [UI modes and the update trigger](analysis/ui-modes-and-update-trigger.md): the "windows" (really modes), events, and every path into the SD update.
- [Games](analysis/games.md): the five built-in games and how the menu finds them.
- [Key scanning](analysis/key-scan.md): how buttons reach the firmware and the multi-key question.

## Updating and reading the flash
- [**How firmware updates work**](updating-the-firmware.md): how to trigger an update, when a file is accepted, what gets written (partial updates work), and what happens to your settings and photos.
- [The update routine](analysis/update-routine.md): the disassembly behind it: the check and its status values, the flash driver, and the write loop.
- [USB modes](analysis/usb-modes.md): the USB interfaces the firmware exposes.
- [Reading the flash without a programmer](experiments/flash-read-without-t48.md): how the USB dumper works, and how far to trust it on other devices.

## Sound
- [Sound](analysis/sound.md): the firmware's WAV resources and playing your own PCM through its audio DMA.
- [The firmware MP3 decoder](analysis/mp3-decoder.md): how the stock decoder is loaded, what it needs in memory, and what it costs in CPU.

## Running your own code
- [SDK and game host](../sdk/README.md): how programs are loaded and run, the `GAME.BIN` and flash-program formats, and how the host hooks into the stock firmware.
- [Doom port notes](../doom/NOTES.md): memory layout, video, controls, sound, performance and what the crashes taught us.
- [Toolchain](../toolchain/README.md): why a custom compiler was needed, how to build it, and its known issues.

## Background and data
- [Earlier work and credits](earlier-work-sprite_tm.md): Sprite_tm's write-up of a camera with the same SoC family, and what we did and didn't take from it.
- [`config/symbols.txt`](config/symbols.txt): the function start addresses found in the firmware (a few are named from evidence).
- [`images/`](images): photos of the camera, its board and Doom running on it.
