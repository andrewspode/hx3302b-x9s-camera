# USB modes

What the camera does over USB. Tags: **[tested]** observed on the camera, **[disasm]** read from the disassembly only.

## Summary
- **[tested]** The camera enumerates as one composite USB device (`349c:3301`, "Generic HD video"): a UVC 1.00 webcam (MJPEG only, up to 1920x1080, with 1280x960, 1280x720, 640x480 and 320x240 modes), a USB audio
  (UAC) microphone, and one mass-storage LUN, "Generic Mass-Storage". Strings in the firmware: `-UVC ON-`, `-UVC OFF-`, `-UAC ON-`, `-UAC OFF-`, `-DUSB MSC TYPE-`, `-DUSB COMBINE TYPE-`, `cbw sig err`.
- **[tested]** The mass-storage LUN exposes the **SD card**, not the internal flash: it mounts as the card's FAT volume, and a plain `READ(10)` of sector 0 does not match the flash header.
- **[tested]** The mass-storage stack has a **vendor SCSI command `0xCC` that calls any firmware address you give it**. That is what `tools/usb_flash_read.py` uses to read the internal flash over USB
  ([how it works](../experiments/flash-read-without-t48.md)).
- **[disasm]** A second vendor command, `0xCE`, sets the real-time clock from the host.
- **[disasm]** There is a `start usb update` string, but the task behind it is never registered, so there is no direct-over-USB firmware update in this build. The SD file (`Xly100s.bin`) is the only firmware write path
  ([how updates work](../updating-the-firmware.md)).

## The mass-storage stack
- **CBW handling.** The 31-byte Command Block Wrapper from the host is byte-swapped field by field into a device struct at `0x02019CC8` (code at flash `0x253F4`-`0x255C8`). The signature is checked against `0x43425355`
  ("USBC"); a mismatch prints `cbw sig err` and stalls. Struct offsets: `+0x30` signature, `+0x34` tag (the host's data-transfer length), `+0x3C` flags/direction, `+0x40` SCSI opcode, `+0x48`... the CDB operands.
- **Command dispatcher** `0x06024E38` branches on the opcode:

  | Opcode | Meaning (standard SCSI unless noted) | Handler |
  |---|---|---|
  | `0x00` TEST UNIT READY, `0x1B` START/STOP, `0x1E` PREVENT/ALLOW REMOVAL, `0x2F` VERIFY | status only | inline |
  | `0x12` INQUIRY | returns the `Generic Mass-Storage ... Beacon` vendor/product string | `0x0607BC9C` copy |
  | `0x03` REQUEST SENSE | builds sense data | inline |
  | `0x1A` MODE SENSE(6), `0x23` READ FORMAT CAPACITIES, `0x25` READ CAPACITY(10) | capacity and parameters | inline |
  | `0x28` READ(10) | reads 512-byte blocks | `0x06024C38` |
  | `0x2A` WRITE(10) | writes 512-byte blocks | `0x06024D38` |
  | `0xCC` **vendor** | calls a host-supplied address | `0x06024B10` |
  | `0xCE` **vendor** | logs `updata rtc:%x`, calls `0x0602CD78` | `0x06024AC8` |

- **READ(10)** reads the LBA and block count, then calls a function pointer at struct `+0x84` with `(lba, buf, 1)` per 512-byte block and streams each block to USB through `0x02000574`. **WRITE(10)** mirrors it
  with a pointer at `+0x88`. These are what the SD card sits behind.
- **`0xCE`** reads one CDB operand, prints `updata rtc:%x` and calls `0x0602CD78(operand)`: it is "update RTC", not a firmware update, and not a flash path.

## The vendor command `0xCC` **[tested]**
The `0xCC` handler (`0x06024B10`) calls a function whose address comes from the command itself. The CBW parser (`0x06025394`) lays the CDB into struct fields; for `0xCC` the host controls these (all little-endian):

| CDB bytes | Struct field | Meaning |
|---|---|---|
| 1-4 | `+0x64` | **the address to call** |
| 5-8 | `+0x68` | source pointer |
| 9-12 | `+0x6C` | optional fill callback (`0xFFFFFFFF` = none) |
| 13-15 | `+0x70` | the callback's third argument |

The handler calls the address as `fn(&struct+0x30, &struct+0x74)` (a pointer to the parsed command block, and a data buffer), and then returns straight to the status stage. The called routine must therefore send any
response data itself.

## How response data returns to the host **[disasm]**
For a data-in command the handler builds its response in the buffer at struct `+0x7C`. `0x060246E8(buf, len)` sends it, transmitting the lesser of `len` and the host's requested length through the bulk-IN primitive
`0x02000574(buf, count)`, and recording any shortfall as the residue in the status wrapper. `0x06024B5C` then builds and sends the 13-byte CSW (`"USBS"`, tag, residue, status). So the data buffer is `+0x7C`, the
count is clamped to the host's length, and `0x02000574(ptr, count)` is the one leaf that sends bytes to the host.

## Reading memory with `0xCC` **[tested]**
The generic data-in sender is `0x06024868`: struct `+0x68` is the source pointer (`-1` means the default buffer at `+0x7C`), `+0x54` is the byte count (initialised from the host's length), and `+0x6C` is an optional fill
callback, called as `callback(source, length, +0x70)`. It then sends the bytes with `0x06024844(source, length)`, a thin wrapper around `0x02000574`. A direct read is therefore `0xCC` with target `0x06024868`, the
source in bytes 5-8 and callback `0xFFFFFFFF`; a 512-byte read at flash offset 0 is the CDB `cc 68 48 02 06 00 00 00 06 ff ff ff ff 00 00 00`.

- **RAM:** direct reads work (512 of 512 bytes matched at `0x02000000`).
- **Flash:** direct reads return **zeros**: the USB engine can't read memory-mapped flash. So the host stages a 72-byte stub through the generic data-out receiver `0x060248D8`. The stub calls the CPU memcpy
  `0x0607BC9C` to copy flash into the USB response buffer and then sends that buffer with `0x06024844`. It lives at SRAM `0x81F0` and reads at most 256 bytes at a time into `0x8040`, keeping code and data
  apart (an earlier layout, 512 bytes at `0x8050`, overwrote the running stub and reset the camera).
- `tools/usb_flash_read.py cc-dump` embeds and verifies the stub automatically. A complete 4 MiB read matched the stock image read earlier with a programmer for every byte below `0x290000`; only the runtime
  settings and photo-store area differed.

These addresses are for the firmware build we tested; see [the limitation](../experiments/flash-read-without-t48.md).

## `start usb update` is compiled out
The task launcher `0x060472E8` looks up a per-id slot in a table at `0x02019270`; if the slot for id `0x0B` is empty it just prints `start usb update` and returns. The registrar `0x060471BC` registers ids
`0`-`5`, `7`-`9` and `0x0C` (the launcher also handles `0x0F`), and never `0x0B`. So requesting the task does nothing: this build has the *name* of a USB firmware-update task but not the task.

## See also
- [How firmware updates work](../updating-the-firmware.md) and [the update routine](update-routine.md): the SD update path.
- [UI modes and the update trigger](ui-modes-and-update-trigger.md): the UI event that reaches USB device mode (event `2` leads to `[COM] usbdev in/out`, mode 10).
