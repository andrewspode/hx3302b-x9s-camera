# How firmware updates work

The camera can reflash itself from a file on its SD card. This is how everything in this repository gets onto the camera, so it is worth understanding properly. It has been **proven on the camera**,
including updates of up to the full 4 MiB; the details below come from reading the update routine's disassembly and from those tests.

> **Warning:** a flash that is interrupted can brick the camera, and only a programmer then recovers it. Start with a fully charged battery, don't touch the camera during the progress bar,
> and keep a full dump of your camera first.

## How to trigger an update
1. Put a file named exactly **`Xly100s.bin`** in the **root** of the SD card. (It is the only `.bin` name the firmware looks for. The `100s` matches the board silkscreen `100S_V4.0`; what `Xly` stands for is unknown, and the name appears nowhere else in the firmware. Sprite_tm's camera used `hx330x_sdk.bin`, so the name varies between products.)
2. Power the camera on **normally**, with the card inserted and the USB cable and charger unplugged. The camera checks for the file on every normal power-on. It also checks at the charging screen when you
   press the power key, and when you hold the shutter there (that second path is how the "TEST ON" label was found).
3. If the file is accepted you see a progress bar, then "bye bye", and the camera powers itself off. Switch it on again to run the new firmware.

If nothing happens, the file was refused or never read. The camera prints the reason only as a debug message, so from the outside "refused" and "not triggered" look identical.

## When a file is accepted
The update routine reads the header of the installed firmware and of your file, each 512 bytes long, and compares two fields: a **checksum** (offset `0x10`) and a **build time** (offset `0x14`). The rules are:

| Situation | Result |
|---|---|
| checksum **or** time differs from the installed one | **update** |
| checksum **and** time are both identical | refuse (`check same ,no to update`) |
| file has no `BLDR` marker at offset 4, or can't be read | refuse |
| installed flash has no valid `BLDR` header, or either side holds the placeholder `0123`+`4567` | update (recovery and force paths) |

So the comparison is **difference, not "newer"** (Sprite_tm's camera wanted a newer timestamp; this one doesn't care). Every new update file therefore needs a header time different from the one currently
installed. The firmware **never computes or checks the checksum**, so a changed file with the old checksum is accepted and boots; the checksum algorithm is still unknown.

## What gets written
- **File offset = flash offset.** The file is written from flash address 0 upward, in whole 4 KiB sectors, and only as far as the file is long. That means **partial updates work**: a file that covers just the first
  part of the flash, up to a change, leaves everything after its end untouched. Pad the file to a multiple of 4 KiB (normally with `0xFF`).
- **Each sector is erased, programmed, then read back and compared** with the file.
- **The header is the commit point.** The first 512 bytes of sector 0 are written as zeros in the first pass, and the real header is written last. A power cut in between leaves a zeroed header, which probably
  doesn't boot from flash (whether the mask boot ROM has an SD fallback is unknown). Keep a full dump and a way to reprogram the chip (an XGecu T48 and a SOIC-8 clip worked for us).
- Nothing in the loop checks the file's checksum field: it writes whatever the file contains.

## What a full update does to settings and photos
The top of the flash holds state that the camera writes while running, and a long update file can overwrite it:

| Flash range | What |
|---|---|
| everything below `0x290000` | firmware and resources: only an update changes these |
| `0x290000`-`0x2901FF` | the **settings block** (512 bytes, ends with an additive 32-bit checksum) |
| `0x291000`-`0x3FFFFF` | the **internal photo store**: a FAT-style volume image used for photos taken with no SD card (the manual quotes "16~51 card-free photos") |

- **Keep update files below `0x290000`** and your settings and photos are left alone. Any ordinary patch is far shorter than that.
- **A longer file overwrites them.** Our Doom update is longer on purpose: it keeps your own settings block and the store's header by copying them from your dump, and uses the rest of the photo store for its code,
  so photos taken without an SD card are lost. Copy them off first.
- The photo store's index format, and exactly when the camera chooses it over the card, are not decoded. Neither is when the settings block gets rewritten (the writer sums a 512-byte RAM buffer, stores the sum in
  the last word, erases the sector and programs 512 bytes; it is called from one large routine and one small one, but we haven't traced which actions reach it).

## Practical rules
- Always keep a full dump of your camera before flashing anything (`tools/usb_flash_read.py`, or a programmer).
- Keep every file you flash, and note its header time; the next one must differ.
- Don't unplug or power down during the progress bar.
- To go back to stock after the Doom update, build a restore file from your own dump: see [`doom/README.md`](../doom/README.md).

## The details
The disassembly behind this page is in [the update routine](analysis/update-routine.md): the check and its status values, the flash driver, and the write loop. The code paths that call it are in
[UI modes and the update trigger](analysis/ui-modes-and-update-trigger.md), and the header fields are in [the flash map](analysis/flash-map.md).
