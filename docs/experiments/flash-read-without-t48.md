# Reading the flash without opening the camera

We wanted a way to dump the firmware **without physically opening the device**: no screws, no SOIC-8 clip, no programmer. We found one, and **we proved it**: `tools/usb_flash_read.py cc-dump` reads the
camera's complete 4 MiB flash over its ordinary USB cable. A programmer (we used an XGecu T48 with a SOIC-8 clip) is still the recovery path if a flash goes wrong, so keep a dump and keep that route open.

Claude declined to help with this part of the work for security reasons, so it was done with Kimi 3.1 instead.

## How it works
The camera's USB mass-storage firmware has a vendor SCSI command, `0xCC`, that **calls any address you give it**. The firmware's own USB routine can read RAM but returns zeros for the memory-mapped flash, so we
turn the camera into its own flash reader.

The host writes a tiny 72-byte program, a stub that copies bytes and sends them back, into the camera's SRAM through the firmware's ordinary data-transfer path, and checks that it arrived intact. It then uses `0xCC`
to run the stub again and again: each run copies a 256-byte chunk of flash into the response buffer and sends it over USB. In effect the camera's own CPU becomes a proxy that reads its flash for us, a chunk at a
time, over a cable that was only ever meant to be a disk.

## The limitation: it has only been tried on one firmware
The method uses **specific memory addresses that we found in one firmware build** (`3030B-20A1-20A1 V1.1`): the helpers the `0xCC` handler calls, where the stub is placed in SRAM, and the buffers it uses. We have
tested it on that build only, so we don't know how widely it works.

It may well work on other units: another X9S with the same firmware version should match exactly, and other products built from the same vendor SDK might share many of the addresses. Against that, addresses
in the main firmware body usually move whenever a build changes, even with the same SDK, so a different product or version could need different ones. There is no way to check beforehand; the only test is to try it.

If the addresses don't line up, the realistic outcome is that it simply doesn't work: the dump fails, or the camera hangs or resets, and a power-cycle fixes it. The tool only ever writes 72 bytes of RAM and never
writes flash. Because it does run code at addresses we haven't checked on that firmware, we can't promise nothing worse, so try it only on a camera whose contents you can afford to recover (keep a programmer route
available), and don't sweep `0xCC` addresses to "find" the right ones. See the warning at the top of `tools/usb_flash_read.py`.

The rest of this page is the working notes behind the method: why a never-dumped device is harder, what is reusable, and the alternatives we considered and didn't need.

The full derivation of the `0xCC` command and the read recipe (handler addresses, the stub's placement, the chunk size and the self-overlap bug we hit) is in [USB modes](../analysis/usb-modes.md).
On the verified run, every byte below `0x290000` matched the stock image read earlier with the T48; the only differences were the runtime settings and photo data above it.

## What is reusable on another device
- The host-side flow: raw SCSI over `SG_IO`, the command layout, staging a stub and reading it back to verify, and the stub's source (`tools/usb_read_stub.c`).
- The idea itself, for any device whose USB engine can read RAM but not memory-mapped flash: use the CPU to copy a small chunk of flash into a buffer the USB engine can see.

What is **specific to the build we tested** is the profile: opcode `0xCC`, sender `0x06024868`, receiver `0x060248D8`, the CPU memcpy `0x0607BC9C`, flash base `0x06000000`, 4 MiB size, response buffer `0x8040`, stub
address `0x81F0` and 256-byte chunks. The embedded stub contains absolute calls to this build's memcpy and sender. For a similar device, the careful route is a short RAM read first (accepting a possible reset),
then analyse that dump offline to find the other build's own sender, receiver, memcpy and buffers. There is no side-effect-free "do you support `0xCC`?" probe, because the command always calls the address you give it.
Dumped firmware may contain personal data (ours held photographs), so review it before publishing anything.

## Why a never-dumped device is harder
Every firmware-based trick needs working firmware on the chip, and most need us to understand it already. The SD update and the EXIF memory-dump patch both *write* flash from offset 0, so a patch has to contain every
byte before the change, including the header and first stage, which we don't have for an undumped device. The `0xCC` method needs the firmware's USB stack to be running and to match. Nothing here is a universal
"read any chip without a programmer" method.

## What else we looked at
- **A plain USB disk read** (SCSI `READ(10)`): tested, and it returns the **SD card**, not the internal flash, so it can't replace `0xCC`.
- **A mask boot-ROM download mode**, the usual route on cheap camera and MP3 chips (hold the flash blank at power-on and see whether the chip enumerates as a different USB device or speaks on a UART). This is the only
  idea that could read a never-dumped device, but we never tried it: the mask ROM isn't in our dump, no public protocol for HX-Tech is known, and Sprite_tm found no working debug UART on his camera.
- **`start usb update`**: a string and a task-launcher case exist, but the task is never registered in this build, so it is a dead end. There is no direct-over-USB flash update in this firmware.
