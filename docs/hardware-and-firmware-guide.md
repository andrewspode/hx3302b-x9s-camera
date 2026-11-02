# Hardware and firmware guide

This is a programming reference for the specific camera and stock firmware (`3030B-20A1-20A1 V1.1`) this repository is about. It describes the device, not our software: our own additions are documented with the code, starting at the [SDK](../sdk/README.md). Addresses are runtime addresses unless a field is explicitly called a **flash offset**.

**Confidence tags**

- **[tested]**: observed on this camera.
- **[disasm]**: read from disassembly or decoded from firmware structures, but the behaviour itself was not necessarily exercised.
- **[inferred]**: deduced from measurements or surrounding behaviour.
- **[guess]**: a useful hypothesis, not something to rely on in code.

A tag at the start of a paragraph or table row covers that whole item.

**Privacy:** flash `0x291000`-`0x3FFFFF` can contain personal photographs (photos taken without an SD card), so this document contains nothing from that range.

## 1. Device, CPU and boot


### Identity

- **[disasm]** SoC: HX-Tech HX3302B; 4 MiB SPI NOR flash, JEDEC ID `C4 60 16`; board `100S_V4.0`; firmware menu version `3030B-20A1-20A1 V1.1 20240701`.
- **[disasm]** USB identity is `349c:3301`, a composite UVC 1.00 MJPEG camera, USB audio microphone, and one mass-storage LUN exposing the SD card. The sensor driver string is `mipi_BF20A1_vga`, which suggests a VGA-class sensor with larger output sizes scaled up; the native sensor resolution has not been measured.
- **[tested]** The project's reference image was a read of an X9S with an XGecu T48 and SOIC-8 clip; two independent reads matched. A T48 file is 4 MiB of flash plus 512 unexplained trailing bytes; only the first 4 MiB belongs in a flash update.

### Instruction set and toolchain

- **[tested]** The CPU is a 32-bit OpenRISC 1000 variant with little-endian instructions/data and **no branch delay slots**. `l.jal` places `pc + 4` in `r9`; the instruction after a call or taken branch is not a delay-slot instruction. Compiled hxsdk code and direct firmware calls run correctly.
- **[tested]** Use Ghidra language `OpenRISC:LE:32:default`, importing the first 4 MiB at `0x06000000`. The cross compiler is `$HOME/tools/or1kle/bin/or1kle-elf-gcc` (binutils 2.43.1, GCC 14.2.0, C and libgcc). Typical freestanding flags are:

  ```sh
  -O2 -fno-pic -mhard-div -mhard-mul -ffreestanding -nostdlib
  ```

- **[disasm]** Do not use PIC or software division: their support code still assumes delay slots. `or1kle-elf-objdump -d` mis-decodes operand fields; use Ghidra for authoritative disassembly. Ghidra's decompiler can omit `mtspr`/`mfspr`, so check hardware-facing routines in raw disassembly.
- **[tested]** There is no usable OR1K tick timer at SPR `0x5001`: reading it reset the camera. Use firmware `now_ms()` instead.
- **[tested]** The simple-instruction benchmark executed 64 M instructions in 780 ms, about 82 M instructions/s. Dependent multiply and divide loops took 617 ms for 4 M iterations and 154 ms for 1 M iterations respectively (about 154 ns per dependent iteration). Other measured speeds are in §2.5.

### Boot flow and address model

- **[disasm]** Flash offset `X` is memory-mapped at `0x06000000 + X`. The 512-byte header starts with a load description and `BLDR` at offset 4. The hidden mask boot ROM is not in the dump.
- **[tested]** Flash `0x200` onward is loaded into 64 KiB SRAM at address 0: runtime address = flash offset - `0x200`. Its first 4 KiB matched exactly; later SRAM contains live state (from an early memory dump).
- **[tested]** Flash `0x2E00`-`0x13FFF` is copied to main RAM `0x02000000`-`0x020111FF`: runtime address = flash offset + `0x01FFD200`. A dump of RAM matches this range apart from 36 bytes of runtime state.
- **[disasm]** The large body at flash `0x14000`-`0x7DFFF` executes in place at `0x06014000`-`0x0607DFFF`. It contains UI, modes, games, FatFs, JPEG, sensor and USB code.
- **[inferred]** The mask ROM and/or first stage performs the loads from the header. No visible firmware copy loop populates the RAM image; the exact division of responsibility is unknown.
- **[disasm]** All code called by the firmware is present in the dump. `0x04000000` is not a ROM window; that idea came from resolving PC-relative calls with the wrong caller base.

**Ghidra offset gotcha.** **[disasm]** When Ghidra analyses a call *from the RAM-copy region as though its caller were at `0x06000000 + flash`*, the displayed absolute target is `actual + 0x04002E00`. Subtract `0x04002E00`: apparent `0x0A01FB60` is really body function `0x0601CD60`, and apparent `0x04003E34` is really SRAM watchdog function `0x00001034`. Calls from the big body directly to the RAM copy appear as `0x02......` and need no such correction. `config/symbols.txt` still uses the superseded run-address model.

## 2. Memory and flash layout


### 2.1 Address windows

| Range | Meaning | Confidence |
|---|---|---|
| `0x00000000`-`0x0000FFFF` | 64 KiB SRAM: first-stage code/data, then live buffers and the UI event-handler stack. `0x10000` and above read as zero in a memory dump. | **[tested]** |
| `0x02000000`-`0x021FFFFF` | 2 MiB main RAM. Reading `0x02200000` reset the camera. | **[tested]** |
| `0x06000000`-`0x063FFFFF` | 4 MiB memory-mapped SPI flash. CPU reads and instruction fetches work. | **[tested]** |
| `0x04000000` region | Not a boot-ROM mapping; CPU and DMA reads returned zero in an early probe. | **[tested]** |

### 2.2 Main RAM regions relevant to programs

| Range | Use and rules | Confidence |
|---|---|---|
| `0x02000000`-`0x020111FF` | Boot-time copy of flash `0x2E00`-`0x13FFF`: updater, flash writer and helpers. | **[tested]** |
| `0x02011000`-`0x0201AFFF` | Firmware state and static buffers. The heap lists, mode table, key thresholds and display descriptors live here. | **[disasm]** |
| `0x0201B000`-`0x0201BFFF` | File-system buffer. File I/O changes it; never lend it to a program. | **[tested]** |
| `0x0201C000`-`0x02022FFF` | Repeatedly DMA-written area. It can rewrite the same values, so fingerprint-only activity scans miss it. Never borrow it. | **[tested]** |
| `0x02023000`-`0x02096FFF` | 464 KiB contiguous, pattern-tested stable region (a program loaded from the Games button used it for code, data and heap). Stable under drawing, palette changes, SD reads/writes, audio and idle in the tested Games workload. | **[tested]** |
| `0x02097340` | Current 320x240, 8-bpp display layer, 76,800 bytes, stride 320. Obtain it through `*(u8 **)0x02019FF8`, not by hard-coding this instance. | **[tested]** DISP.BIN |
| `0x020A9F40` | Rotated 240x320 copy for the portrait panel in the observed instance. | **[tested]** DISP.BIN |
| `0x020BCB40`, `0x020CF740`, `0x020E2340` | Three further 76,800-byte display copies, exactly 76,800 bytes apart. The last ends at `0x020F4F40`. Never use these for stacks or heap. | **[tested]** |
| `0x020F5000`-`0x02108FFF` | A program run from flash can use this 80 KiB. It is **not generally free static RAM**: Games' allocated screen buffer was `0x020F6A00` + `0x12000`, inside it. Use only with the matching host/layout. | **[tested]** |
| `0x02109600` upward | Firmware heap in the observed game-window layout. Its exact blocks change with allocations; query `largest_free()` and walk the lists rather than assuming an end address. | **[tested]** |
| `0x021E4000`-`0x021FFFFF` | Do not write this as a wholesale spare region: a memory test reset the camera during the fill. This does not mean every tail byte is permanently allocated; a later heap survey found a small free heap block near the top. | **[tested]** |

- **[disasm]** Firmware heap free-list root: `*(u32 *)0x0201327C`; used-list root: `*(u32 *)0x02013278`; spare descriptors: `*(u32 *)0x02013274`. Descriptor fields are `+0 used`, `+4 block address`, `+8 size`, `+0x10 next`.
- **[tested]** Firmware `malloc` returns 64-byte-aligned blocks and sizes are rounded to 64 bytes. In the original fly window approximately 889 KiB was available. When the flash program starts from Games, a heap survey found 896,000 free bytes in two blocks, largest 891,008 bytes; this is a snapshot, not an ABI guarantee.
- **[tested]** A large program needs a private stack allocated from the firmware heap: a statically placed stack at `0x020D1000` was overwritten by display pixels, which caused crashes in early runs of a large test program.

### 2.3 Display globals

| Address | Contents | Confidence |
|---|---|---|
| `0x02019FF0` | Layer object pointer (`0x0201ACD8` in DISP.BIN). | **[tested]** |
| `0x02019FF4` / `0x02019FF6` | `u16` layer width/height: 320/240. These are the pointers in the services table. | **[tested]** |
| `0x02019FF8` | Pointer to 8-bpp layer buffer. | **[tested]** |
| `0x0201A1A4` / `0x0201A1A6` | `u16` portrait panel width/height: 240/320, **not** layer dimensions. | **[tested]** |
| `0x02019FFC` | Pointer to a rotated 240x320 copy of the layer (stride 240) for the portrait LCD panel; the 320x240 layer buffer at `0x02019FF8` is the one programs draw into. | **[tested]** |
| `0x020192EC` | Games-mode allocated screen buffer pointer; it overlaps the second RAM area used by a program run from flash. | **[tested]** |

The stock layer palette (resource 64) has these notable entries: 0 = light blue (24, 214, 255) opaque, 1 yellow, 2 white, 7 black, 251 = 50% black (the shadow), 253 = black, and 254 and 255 and about 50 unused slots clear (alpha 0).
In the stock windows the layer is 8-bit with index 0 shown as the window's background picture, and the whole thing is hardware-scaled to the portrait panel. **[tested]** (a custom palette can be shown and the stock one restored).

### 2.4 Flash map

The flash layout (header, first stage, RAM image, main body, tables, MP3 overlay, resources, settings and the internal photo store) and the header fields are in [`analysis/flash-map.md`](analysis/flash-map.md).

### 2.5 Measured memory and XIP speed

- **[tested]** Both CPU caches are **8 KiB** (`sdk/hxsdk/cachebench.c`). Reads and writes of a working set up to 8 KiB took 51 ms per 8 MiB (about 164 MB/s, limited by the instruction rate rather than the memory); at 10 KiB they were partly slow (98 ms read, 157 ms write) and from 12 KiB fully slow: reads 128 ms (about 65 MB/s), writes 227 ms (about 37 MB/s). Straight-line code ran at about 120 M instructions/s up to 8 KiB (278 ms per 32 M), 4.3x slower at 12 KiB (1,200 ms) and about 6x slower from 16 KiB (1,660 ms, about 19 M instructions/s), so the instruction cache is 8 KiB too.
- **[tested]** The 82 M instructions/s figure is a 4-instruction loop with a taken branch; straight-line code with no branches reaches about 120 M/s. Both fit one instruction per cycle at roughly 120 MHz with a taken branch costing two extra cycles, but the clock itself has not been read from the hardware **[inferred]**.
- **[tested]** An earlier benchmark (`BENCH.TXT`) gave 127 MB/s for reads inside the cache and 59 MB/s outside, and 164 / 37 MB/s for writes; the write figures match the later run, the read figures do not. The later run calls the watchdog function far less often, which is the likely cause of the difference **[guess]**. Other results from that benchmark: firmware `memcpy` 22.5 MB/s, flash data read 4.4 MB/s, SRAM read 134 MB/s (16 MiB or 4 MiB totals as defined in `sdk/loader/bench.c`).
- **[tested]** Cache-resident code runs at the same speed from flash and RAM: 1,880 vs 1,881 ms and 2,577 vs 2,577 ms in two benchmark loops. About 100 KiB of straight-line code took 55.146 s from flash vs 3.017 s from RAM, roughly 18x slower; flash instruction line fills were about 3.6 MB/s. Sequential flash data reads were about 8x slower than RAM, and one word per 64-byte cache line about 17x slower (`FLASHBEN_run1.TXT`).
- **[inferred]** Keep hot code and hot read-only data in RAM. Flash is appropriate for cold code and cold constants, not for a renderer's inner loops or large cold-streamed tables.

## 3. Callable firmware services


### 3.1 Core, heap, cache and display

| Address and C signature | Behaviour and caveats | Confidence |
|---|---|---|
| `0x0607BC9C void *memcpy(void *dst, const void *src, u32 n)` | CPU copy; works on memory-mapped flash. Do not replace it with DMA for flash reads. | **[tested]** |
| `0x0602DC20 void *malloc(u32 n)` | Firmware heap allocation; 64-byte alignment/rounding. | **[tested]** |
| `0x0602DE64 void free(void *p)` | Firmware heap free. | **[tested]** |
| `0x0602E040 u32 largest_free(void)` | Largest free heap block in bytes. | **[tested]** |
| `0x02004524 void dcache_writeback(void *p, u32 n)` | RAM copy of flash `0x7324`; pushes dirty CPU data to RAM before hardware/DMA reads it. | **[tested]** |
| `0x060691EC void dcache_invalidate(void *p, u32 n)` | **Invalidate/discard**, not write-back. Used after DMA fills RAM. Misusing it after self-relocation discarded patched instructions and rebooted the test program. | **[tested]** |
| `0x060073C0 void sd_dcache_invalidate(void *p, u32 n)` | SD driver's pre-DMA invalidate path. Treat unaligned neighbouring dirty data as at risk. | **[disasm]** |
| `0x060691A8 void icache_reset(void)` | Disable/reset/re-enable instruction cache after loading or patching code. | **[tested]** |
| `0x06039198 void present(void)` | Presents the current layer; performs its own display-buffer cache write-back. | **[tested]** |
| `0x060660DC void set_palette(u32 layer, const u32 *pal)` | Sets 256 entries for layer 0. Entry = `(alpha5 << 16) | RGB565`; alpha `0x1F` opaque, `0x10` half, 0 clear. | **[tested]** palette chart |
| `0x0604196C void palette_restore(void)` | Allocates/loads resource 64, restores layer 0 stock palette and frees the temporary buffer. Useful after a program has changed the palette. | **[tested]** |
| `0x0602F6AC u32 now_ms(void)` | Interrupt-driven millisecond counter. | **[tested]** |
| `0x00001034 void wdt_kick(void)` | Feeds watchdog when enabled. Call in long compute/wait loops. | **[tested]** |

- **[inferred]** Pure RGB565 black may act as a transparent colour key: a zero-black palette produced magenta bands, and mapping zero entries to RGB value 1 avoided it. Alpha 0 is definitely transparent; index/value transparency in the stock palette is not a general licence to treat every zero pixel as transparent.

### 3.2 Files and resources

| Address and C signature | Behaviour and caveats | Confidence |
|---|---|---|
| `0x0601CD60 int f_open(const char *name, int mode)` | Modes: 1 read, 3 read/write, `0xB` read/write/create-always. Returns firmware fd 0-3 or negative. Four fds maximum. Avoid opening the same file twice concurrently; that crashed the file layer. | **[tested]** |
| `0x0601CEB0 int f_read(int fd, void *dst, u32 n)` | Returns bytes read or negative. Destination cache alignment and sector alignment matter; see §4.1. | **[tested]** |
| `0x0601CF28 int f_write(int fd, const void *src, u32 n)` | Writes bytes; used for logs and tests. | **[tested]** |
| `0x0601CE38 void f_close(int fd)` | Closes fd. | **[tested]** |
| `0x0601D3C8 u32 f_size(int fd)` | File size in bytes. | **[tested]** |
| `0x0601D018 int f_seek(int fd, u32 off_lo, u32 off_hi, u32 flags)` | 64-bit offset in two words; current code passes high=0, flags=0. Negative means failure. | **[tested]** |
| `0x0603817C int resource_read(u32 id, void *dst)` | Loads a resource into caller storage; used with resource 64 palette. | **[tested]** through palette restore |
| `0x0601F98C void *resource_address(u32 id)` | Locates resource-area address. | **[disasm]** |
| `0x0601FA08 u32 resource_size(u32 id)` | Resource size. | **[disasm]** |
| `0x0601FA88 void resource_copy(void *address, void *dst, u32 size)` | Copies a located resource to RAM. | **[disasm]** |

Resource IDs used by menu data are `0x81000000 | n` for UI string `n` and `0x80000000 | n` for image `n` **[disasm]**.

### 3.3 UI, input and modes

| Address and C signature | Behaviour | Confidence |
|---|---|---|
| `0x0602F36C void post(void *queue, u32 message)` | Interrupt-safe ring post; drops when full. UI queue is `*(void **)0x0201A014`; message = `id | (param << 16)`. | **[tested]** |
| `0x0601450C int device(int id, u32 command, void *arg)` | Device dispatcher. `device(3, 0x400, &event)` retrieves packed key events. | **[tested]** |
| `0x020005B0 int adc_read(u32 channel)` | Raw 10-bit ADC sample. Channel 3 reads the five-key resistor ladder. | **[disasm]** wrapper and SPR mask; raw combinations untested |
| `0x060472E8 void mode_request(u32 id, u32 arg)` | Requests a mode transition. Mode 1 powers the camera off; mode 2 is the main screen. | **[tested]** |
| `0x060472C8 u32 mode_current(void)` | Returns current mode. | **[disasm]** |
| `0x06047114 void mode_register(u32 id, void *descriptor)` | Registers `{name,arg,enter,exit,service}` in mode table `0x02019270`. | **[disasm]** |
| `0x060486E8(void)` and `0x06048648(void)` | Called with 1 to reset auto-power-off and screensaver idle timers respectively. | **[disasm]** |
| `0x060480DC` | Global generic key handler: beep/backlight and idle-timer handling for `0x14`-`0x19`. | **[disasm]** |

### 3.4 Audio

| Address and C signature | Behaviour and caveats | Confidence |
|---|---|---|
| `0x060385AC int play_resource(u32 id, u32 wait, u32 volume)` | Stock WAV player. Returns 0 on success, -1 failure. Use `wait=0` in the host's own loop; wait mode polls the firmware audio task. | **[tested]** |
| `0x06038554 void stop_resource(void)` | Stops high-level resource playback and frees its WAV allocation. | **[tested]** |
| `0x06027804 int audio_start(u32 rate, const void *pcm, u32 bytes)` | Starts mono signed little-endian PCM16 DMA. Returns 1 for a supported rate, 0 otherwise. Write back PCM first and keep it alive. | **[tested]** |
| `0x02002B38 void audio_queue(const void *pcm, u32 bytes)` | Sets next DMA address/count. Queue from the callback on bit 2 for continuous streaming. | **[tested]** |
| `0x06027860 void audio_stop(void)` | Stops DMA/codec path and sets volume to zero. **Call on callback bit 4**; merely setting a flag led to a reboot, probably repeated interrupt starvation. | **[tested]** |
| `0x06027888 void audio_volume(u32 volume)` | 0-100, clamps above 100. Set before every start because stop zeroes it. | **[tested]** |
| `0x060278E8 void audio_callback(void (*fn)(u32 bits))` | Registers the direct engine callback. Callback runs in interrupt context; only queue and do short interrupt-safe work. Restore the old callback before returning to firmware audio. | **[tested]** |
| `0x0602FA54 void audio_task_stop(void)` | Broader firmware audio-task teardown; not a normal per-buffer stop. | **[disasm]** |
| `0x06048790 void key_beep(void)` | Plays prepared resource-3 key beep through firmware audio path. | **[disasm]** |
| `0x06027718 void audio_init(void)` | Firmware initialises once. Do not call again from a program: it resets codec, volume and callback. | **[disasm]** |

- **[disasm]** Supported exact rates: 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 Hz. Use nonzero lengths, preferably multiples of four bytes; DMA word/sample counts are shifted down.
- **[tested]** Event semantics after the sound tests: bit 2 = current buffer exhausted and engine moved on, so queue the next buffer now; bit 1 = a queued buffer finished; bit 4 = nothing remains, stop the engine. Four 250 ms buffers chained into one continuous second with one final bit-4 event (`SOUND_run3.TXT`).
- **[disasm]** Audio ISR body `0x06005864` executes from RAM `0x02002A64`, handles one status bit per interrupt and acknowledges through SPR `0x914C`. It calls the pointer at `0x02019784`, which holds a dispatcher that in turn calls the user callback stored at `0x02012D8C` (see Audio engine); they are two layers, not alternatives.

### 3.5 SPI flash and USB helpers

These are mainly useful to the updater and USB reader, not normal game code.

| Address and C signature | Behaviour | Confidence |
|---|---|---|
| `0x06003BF0 erase_4k(u32 flash_address)` | SPI opcode `0x20`, address masked to 4 KiB boundary. | **[tested]** through updates |
| `0x06003A7C` / `0x060039E8` | Page-program paths using opcode `0x02`, 256-byte pages. | **[tested]** through updates |
| `0x06003E84 spi_read(...)` | SPI opcode `0x03` flash read; derive full argument list before direct use. | **[disasm]** |
| `0x06024868 cc_sender(...)` | Generic `0xCC` data-in sender; direct RAM reads work, direct flash reads returned zero. | **[tested]** |
| `0x060248D8 cc_receiver(...)` | Generic `0xCC` data-out receiver used to stage the small SRAM stub. | **[tested]** |
| `0x06024844 usb_send(void *p, u32 n)` | Thin bulk-IN send wrapper used by the stub. | **[tested]** |
| `0x02000574 usb_bulk_in(void *p, u32 n)` | Low-level USB bulk-IN leaf. | **[disasm]** |
| `0x0602CD78 rtc_set(u32 value)` | Target of vendor SCSI `0xCE`, apparently set RTC from host. | **[inferred]** |

## 4. Hardware and driver rules


### 4.1 SD DMA and caches — mandatory read rule

- **[disasm]** The SD driver invalidates the destination cache range before DMA. Invalidate discards dirty lines; it does not write them back. A destination that is not cache-line aligned can therefore lose neighbouring CPU-written data.
- **[disasm]** Cache control uses vendor SPRs (meanings inferred from use): data-cache control `0x1800` (bit 0 enables), cached region base `0x1808` = `0x02000000`. Write-back is `0x06007324` (SPR `0x180C`, run from its RAM copy `0x02004524(addr, len)`), explicit invalidate is `0x060691EC` (SPRs `0x1804`/`0x181C`, used after hardware such as the JPEG encoder writes a buffer), and the SD driver invalidates with `0x060073C0` (SPR `0x1814`). After loading code: write back the data range, then reset the instruction cache.
- **[disasm]** FatFs handles a read starting mid-sector by CPU-copying the first part from its window, then DMAing following sectors to `destination + offset`. The following unaligned invalidate can discard the bytes just copied. This explains corrupted data after reads that start mid-sector and file-dependent crashes.
- **[tested]** The robust read pattern is: round the file position **down to a 512-byte boundary**, seek there, read only whole 512-byte sectors into the start of a 64-byte-aligned bounce buffer, then CPU-copy the requested bytes out.
- **[inferred]** Apply the same conservative rule to arbitrary buffers: firmware malloc alignment alone does not make a mid-sector file position safe. Do not read a WAD directly into an arbitrary zone address.
- **[tested]** CPU writes to code or DMA-source PCM need `dcache_writeback`; newly DMA-filled code needs `dcache_invalidate`, then `icache_reset`. Self-relocation additionally writes back before resetting icache.
- **[tested]** DMA reads main RAM, but direct DMA/USB reads of memory-mapped flash returned zeros. Copy flash to RAM with CPU `memcpy` or explicit CPU loads before giving it to a DMA engine (USB dump and host's flash-program copy).

### 4.2 Keys

- **[tested]** Event low 16 bits are key ID; high 16 bits are 0 press, 1 held repeat, 2 release. IDs: `0x14` shutter/OK, `0x15` up, `0x16` down, `0x17` left, `0x18` right, `0x19` "back" (this camera has no back button: it is a short tap of the power button), `0x1A` power held. Repeats begin after about 500 ms and then occur every 130 ms; taps are about 150-180 ms (measured on the camera).
- **[disasm]** Shutter and directions share ADC channel 3, one 10-bit scalar sample. Back/short power and long power use a separate digital path. The stock decoder emits one current key, not a simultaneous-key bitmask.
- **[tested]** Diagonals do not work through stock events. Selected pairs might have distinct resistor-ladder voltages, but no pair ranges have been measured. Poll `adc_read(3)` to research them; do not promise arbitrary multi-key input.
- **[disasm]** Window handlers receive `(win, n, &param)` and normally act when `n == 1 && param == 0`. The parameter pointer can be SRAM (`0x6FA8` observed), so do not reject non-main-RAM pointers.

### 4.3 Audio engine

- **[tested]** The practical interface is one mono PCM16 stream with software mixing. Start one buffer, queue following buffers with `0x02002B38` on bit 2, stop on bit 4, and restore volume before each fresh start. Restarting with `audio_start` for every buffer produced a slight click; queued buffers sounded continuous.
- **[tested]** Unsigned 8-bit 11025 Hz samples work after conversion to signed PCM16 (a 5,660-sample sound effect played correctly for 513 ms).
- **[disasm]** The callback has two layers. The audio ISR calls the pointer at `0x02019784`, which audio init sets to a RAM dispatcher at `0x020005EC` (flash `0x33EC`); the dispatcher loads the user callback from `0x02012D8C` (written by `0x060278E8`) and calls it with the event bits.
- **[disasm]** The engine initialisation is owned by firmware. Only one user should drive it at a time; do not concurrently use key beep/resource playback and a custom mixer.
- **[guess]** A constant ring of buffers, including silence, is the mixer design that works; whether the amplifier gates true digital silence has not been established.

### 4.4 Watchdog and time

- **[tested]** Any loop that runs for a long time must call `wdt_kick`, including frame loops, sound loops and delay loops.
- **[tested]** `now_ms` is suitable for game time and delays. Do not read the unimplemented tick-timer SPR.

## 5. UI modes, windows and events


### 5.1 Modes

- **[disasm]** Mode table base is `0x02019270`. A descriptor is five dwords `{name, arg, enter, exit, service}`; current/requested/previous fields are at table `+0x40/+0x44/+0x48`.

| ID | Name | Enter address | Confidence |
|---:|---|---:|---|
| 0 | Bat Charge | `0x06047514` | **[disasm]** |
| 1 | Power Off | `0x0605793C` | **[tested]** |
| 2 | main | `0x060526A8` | **[disasm]** |
| 3 | Record Video | `0x06059568` | **[disasm]** |
| 4 | Record Photo | `0x06057BB4` | **[disasm]** |
| 5 | Play Video | `0x06055204` | **[disasm]** |
| 7 | C_Game | `0x0604A58C` | **[disasm]** |
| 8 | Play mp3 | `0x060531F0` | **[disasm]** |
| 9 | Setting | `0x0605B8B8` | **[disasm]** |
| 10 | usb device | `0x0605B95C` | **[disasm]** |
| 12 | SD Update | `0x0605B764` | **[disasm]** dead entry in this build |

- **[disasm]** Nothing requests mode 12. Update checks instead run during normal power-on, on the charge screen's power key, and after more than 30 held-shutter repeats on that screen. The last sets the TEST ON flag and enters Record Video.
- **[tested]** A changed `Xly100s.bin` updates during plain power-on; no hidden menu trigger is needed (proven on the camera). A card inserted while already on the charge screen is not noticed by the normal poll; insert before power-on.

### 5.2 Window events and timers

- **[disasm]** Window event tables are `{u32 id, u32 handler}` pairs ending `{0x1B,0}`. IDs `0x09/0x0A/0x0B` are open/close/child-close. The firmware service routine posts `0x06` every 500 ms, `0x11` every 100 ms and `0x12` every second.
- **[tested]** The firmware never posts window event `0x0D`, so it is free to use as a private message. The firmware's own main loop costs about 38 ms per frame, so a program that needs speed should run its own loop instead of returning to it between frames.
- **[disasm]** Global event table `0x060BD5E4` maps `0x14`-`0x19` to generic key handling and `0x1A` to power handling. USB event 2 with param 4 enters USB device mode.
- **[disasm]** Auto-power-off and screensaver time globals are `0x020192D0` and `0x020192CC`. A program that does not generate key events lets these timers blank the screen or power the camera off; turning both off in Settings is the tested workaround. They are not serviced while a program runs its own loop.

## 6. Firmware updates, settings and USB reading


### 6.1 SD update

How to trigger an update, when a file is accepted and what gets written are in [How firmware updates work](updating-the-firmware.md). In short: `Xly100s.bin` in the SD root, a header time that differs from the installed one, a plain power-on; file offset equals flash offset, whole 4 KiB sectors, the header written last. **[tested]**

Header fields decoded so far (512-byte sector units where shown) **[disasm]**:

| Offset | Value/meaning |
|---|---|
| `0x04` | `BLDR` magic |
| `0x09` | Version-block offset divided by 16 (`1` means `0x10`) |
| `0x10` / `0x14` | Checksum `0x315B3CE1` / original build time `0x668258DC` |
| `0x18` / `0x1C` | Resource start `0x803`, length `0xC7B`: flash `0x100600`, length `0x18F600` |
| `0x20` | RAM image destination `0x02000000` |
| `0x24` / `0x28` | RAM image source `0x17`, length `0x89`: flash `0x2E00`, length `0x11200` |
| `0x34` | Constant `0x01234567` |
| `0x38` | `0x800` sectors = flash `0x100000`, the unanalysed small block |
| `0x3C`-`0x48` | Unknown; possible SPI/clock configuration **[guess]** |

### 6.2 Runtime flash writes and settings

The settings block (`0x290000`-`0x2901FF`) and the internal photo store (`0x291000` to the end) are written by the camera while it runs; see [How firmware updates work](updating-the-firmware.md). **[tested]** locations; **[disasm]** the settings writer (additive checksum in the last word, `0xAA55AA55` if the sum is zero). The store's index format and when the camera writes either area are not decoded.

### 6.3 USB mass storage and flash reading

- **[tested]** Ordinary USB READ(10) exposes the SD card, not internal flash. The `start usb update` task ID `0x0B` is not registered in this build, so no working direct USB firmware updater was found **[disasm]**.
- **[disasm]** Vendor SCSI `0xCC` takes a little-endian call target from CDB bytes 1-4, source from 5-8, optional fill callback from 9-12 and callback argument from 13-15. Handler `0x06024B10` calls the target; an invalid address can crash/reset the camera. `0xCE` appears to set RTC.
- **[tested]** Direct `0xCC` reads through sender `0x06024868` work for RAM but return zeros for flash. The working reader stages a verified 72-byte CPU-copy/send stub at SRAM `0x81F0`, reads at most 256 bytes into `0x8040`, and uses receiver `0x060248D8` for staging.
- **[tested]** This command completed a full 4 MiB read:

  ```sh
  sudo python3 -I tools/usb_flash_read.py cc-dump /dev/sgN firmware/dump.bin
  ```

  Every byte below `0x290000` matched the then-current `dump.bin`; differences above it were runtime data. The reader writes 72 bytes to SRAM, not flash or SD.
- **[disasm]** These addresses and the embedded stub are a profile found on this one firmware build; whether other builds share them is unknown (untested). Do not sweep `0xCC` addresses on another device. Review/sanitise any new dump before publication.

## 7. Resources and stock games


### 7.1 Resource directory

- **[disasm]** Directory at flash `0x100600`: 65 entries `{u32 offset_from_0x100600, u32 size}`, ending at `0x100808`.

| IDs | Contents | Confidence |
|---|---|---|
| 0-7 | Eight mono PCM16 RIFF/WAVE resources. Resource 3 is the key beep. | **[disasm]**; playback **[tested]** |
| 8-59 | 52 JPEG backgrounds, frames and game artworks. | **[disasm]** |
| 60 | Eight-byte resource/build date string. | **[disasm]** |
| 61 | 885-glyph, 24-pixel-high 1-bpp font; per-glyph `{u16 width,u16 height,u32 offset}`, MSB-first rows. | **[disasm]** |
| 62 | `FON` tables: 18 languages x 183 strings, stored as `u16` glyph codes with width/height/length/offset records. Thai combining marks carry high flag bits. | **[disasm]** |
| 63 | 220 icons with `{offset,width,height}` records, mostly 32x32. | **[disasm]** |
| 64 | 256 x 4-byte stock layer palette. | **[tested]** |

- **[tested]** To shorten a UI string in place, replace glyph codes and update its length and pixel width. Only glyphs already in the font are available. English string 160 was changed from “Systerm Volume” to “System Volume” and verified on camera.
- **[disasm]** Language block 0 is English. Nineteen language names occur for eighteen blocks; the remaining block mapping is unverified.

### 7.2 Stock game layout

- **[disasm]** Game menu at `0x060BDB04` has five 40-byte entries. Code ranges (flash offsets): 2048 `0x4AC00`-`0x4CAD0`, fly `0x4CAD8`-`0x4D9D0`, Maze `0x4D9D4`-`0x4E3D8`, PushBox `0x4E3DC`-`0x50B80`, snake `0x50B84`-about `0x51700`; menu code `0x4A58C`-about `0x4AC00`. All five are real menu entries; no hidden unfinished game was found.

## 8. Open questions

| Unknown | How to find out | Confidence about the gap |
|---|---|---|
| Header checksum algorithm and exact ROM coverage | Keep analysing candidate algorithms offline; to test the ROM's behaviour, flash a 4 KiB update that changes only the checksum, then a separate changed-body control (with a programmer recovery path ready). | **[disasm]** algorithm unknown; unchanged checksum with changed body **[tested]** |
| Header `0x3C`-`0x48`, block at `0x100000`, MP3 overlay address | Trace the first-stage and header consumers and the MP3 task setup in the disassembly. | **[disasm]** |
| The extra 512 bytes in a programmer read | Compare their behaviour across repeated programmer reads; never append them to an update. | **[guess]** programmer/read artifact |
| Mask-ROM recovery/download protocol | Look for public silicon documentation first; probing a blank-flash chip's USB enumeration or UART is a later step. | **[guess]** no protocol found |
| Settings fields, and the photo store's index and write triggers | Decode the settings writer and reader call sites and use test values; find the internal-photo call paths. | **[disasm]** locations known, formats incomplete |
| Heap used-list layout after a program starts | Run a heap survey that lists the used blocks (not yet done). | **[tested]** partial |
| Identity and purpose of the DMA at `0x0201C000`-`0x02022FFF` | Trace its source addresses and DMA SPR programming; use pattern guards under a targeted workload. | **[tested]** effect, **[guess]** source unknown |
| Simultaneous key pairs | Log raw `adc_read(3)` at frame rate for each single key and pair; accept only stable disjoint voltage ranges. | **[disasm]** scalar interface; combinations untested |
| Exact display semantics of black as a transparent key | Draw adjacent patches with RGB565 0 vs 1 and alpha values 0/`0x10`/`0x1F`, over a known JPEG background. | **[inferred]** from magenta zero-black |
| Whether `0x02023000` is safe outside the Games mode | Repeat the pattern tests in each firmware mode; the clean results so far apply to the Games workload (a large program run from the Games button) only. | **[tested]** limited workload |
