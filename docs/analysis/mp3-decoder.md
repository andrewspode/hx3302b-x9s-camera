# The firmware MP3 decoder

The camera's firmware contains an MP3 decoder (mode 8 loads it). These notes cover how it is loaded and called, what it needs in memory, and how much CPU it costs, as found from the disassembly and an offline emulation. Body addresses are flash offsets plus `0x06000000`; the MP3 overlay is linked at `0x00008000` and its data at `0x00200000`.

## How mode 8 loads it

**[disasm]** Mode 8 entry `0x060531F0` calls `0x060749B4` at `0x06053274`. That function makes two calls to the RAM-resident flash-read helper `0x02000B5C` (RAM copy of flash `0x395C`):

| Source flash offset | Destination | Length | Purpose |
|---|---:|---:|---|
| `0x0D0000` | `0x00008000` | `0x7540` = 30,016 | Decoder code, ending at `0x0000F53F` |
| `0x0D7600` | `0x00200000` | `0x31A8` = 12,712 | Tables/data, ending at `0x002031A7` |

**[disasm]** There is a `0xC0`-byte gap between the code and data source ranges. The relative calls prove the code link address: for example, the instruction at flash `0xD0048`, when executed at `0x8048`, reaches the real debug-print function `0x02001680`; internal calls similarly land at `0x8310`, `0xEF44`, and so on. The `0x00200000` data address is explicit in the `l.movhi r4,0x20` at `0x060749EC`; it must not be transcribed as main RAM `0x02000000`.

**[disasm]** After the copies, `0x060749B4` clears the 44-byte control structure at `0x00207EDC` and allocates:

| Control field | Allocation | Requested size | Use |
|---|---:|---:|---|
| `0x00207EF0` | main heap | `0x48000` = 294,912 | PCM ring storage |
| `0x00207EFC` | main heap | `0x0E48` = 3,656 | Stream state and 128 queue descriptors |
| `0x00207F00` | main heap | `0x1220` = 4,640 | Decoder/input context |
| `0x00207F04` | main heap | `0x1028` = 4,136 | LRC lyrics state |

**[disasm]** The requested main-heap total is `0x4B090` = 307,344 bytes (300.1 KiB), before allocator rounding, optional lyrics-file contents and optional ID3 text allocations. `0x0607490C` stops playback and releases the major allocations; `0x0607494C` is the fuller teardown.

**[disasm]** The mode-8 load path does not reference the rest of flash `0xDA7A8`-`0xFFFFF`. That remaining area may contain related material, but it is not needed by this decoder load and should not be counted as one 192 KiB contiguous overlay.

## Decoder operations and input interface

**[disasm]** The useful operations are:

| Address (overlay address; flash source where useful) | Operation |
|---|---|
| `0x060749B4` | Load the overlay and allocate the stock player state |
| `0x06074AF4(mp3_fd, lrc_fd)` | High-level FAT-file initialisation; also pre-decodes up to 64 frames through `0x060754F8` |
| `0x0000EF44(ctx, work, arg)` (flash `0xD6F44`) | Initialise the input side of a decoder context |
| `0x0000E83C(ctx + 0x6C, work + 0x458, 12, 0x80C)` (flash `0xD683C`) | Initialise the bit reader |
| `0x000080D0()` (flash `0xD00D0`) | Return ID3v2 byte count, or negative if absent |
| `0x00008174(count)` (flash `0xD0174`) | Skip ID3 data through the input abstraction |
| `0x00008310()` (flash `0xD0310`) | Decode one frame into the PCM queue; zero is success, negative is failure |
| `0x00008000()` (flash `0xD0000`) | Player service/pump; mode 8 calls it at `0x06053418` |
| `0x06075210` | Stop the direct audio engine and clear its callback |

**[disasm]** `0x0000EF44` installs these fields at the start of `ctx`:

| Offset | Meaning |
|---|---|
| `+0x00` | Read callback, initially `0x0601CEB0` (`f_read`-like) |
| `+0x04` | Seek callback, initially `0x0601D018` |
| `+0x08`, `+0x14`, `+0x18` | First 1,024-byte input buffer, `work + 0x58` |
| `+0x0C` | Second 1,024-byte input buffer, `work + 0x458` |
| `+0x1C` | Caller argument passed to read/seek (the file descriptor in the stock player) |
| `+0x20` | Buffer count, 1 |
| `+0x24` | Buffer size, `0x400` |

**[disasm]** Refill reaches the read callback indirectly at flash `0xD6CD4` (overlay `0xECD4`); seek reaches the callback at flash `0xD7228` (overlay `0xF228`). The input layer has two 1 KiB buffers and does not require the compressed stream to be one contiguous allocation.

**[inferred]** A program can therefore decode an MP3 buffer it supplies: initialise with `0x0000EF44`, replace `ctx+0` and `ctx+4` with memory-read/memory-seek callbacks, and use `ctx+0x1C` for a pointer or small memory-stream descriptor. A read-only, non-seeking player only needs the seek callback for initial positioning/tag handling and can implement it as an index update. This avoids FAT, LRC and the stock 64-frame pre-decode, but it has not been run on the camera.

## PCM output and the audio path

**[disasm]** Frame information in the decoder context includes:

| Offset | Meaning |
|---|---|
| `+0xD9` | `3 - MPEG_channel_mode`; zero is mono (MPEG mode 3), nonzero selects two channels at `0x06075098`-`0x060750E0` |
| `+0xE4` | Samples per channel per frame: 1,152 for the emulated MPEG-1 stream, 576 for the emulated MPEG-2 stream |
| `+0xE8` | Output sample rate in Hz |
| `+0xEC` | Bit rate in bits/s |
| `+0x214` | Channel count, derived from `+0xD9` |
| `+0x216` | Samples per channel, copied from `+0xE4` |
| `+0x218` | PCM width, 16 bits |
| `+0x21C` | Current synthesis output pointer |

**[disasm]** The synthesis clips to signed 16-bit and stores with native little-endian halfword writes (for example flash `0xD50F8`-`0xD512C` and `0xD620C`). `0x0607508C(pcm, count, ...)` copies `2 * count` bytes into the queue, and `0x060750FC` publishes a queued buffer. For mono (MPEG channel mode 3, `ctx+0xD9 == 0`) this is exactly the signed PCM16 mono format used by `0x06027804`. Stereo handling was not needed for this recommendation and should not be assumed to be interleaved without further work **[inferred]**.

**[disasm]** On the player service call, `0x06075308` obtains a queued PCM buffer, sets the volume, registers `0x06074DF8` and calls:

```c
0x060278E8(0x06074DF8);
0x06027804(ctx->sample_rate, queued_pcm, queued_byte_count);
```

**[disasm]** Callback `0x06074DF8` queues more PCM through `0x02002B38` on event bit 2, uses `0x02002B58` for the final queued buffer on bit 1, and stops through `0x06027860` on bit 4. This is the same direct engine and chaining model as a custom program would use, not the descriptor/task path whose state begins at `0x02013818`. The camera-tested bit semantics and continuous chaining are in `docs/analysis/sound.md:189-206` and the sound tests **[tested]**.

## RAM footprint

**[disasm]** The fixed footprint while loaded is:

| Region | Extent | Bytes / meaning |
|---|---|---:|
| SRAM code | `0x00008000`-`0x0000F53F` | 30,016; upper SRAM, below the 64 KiB boundary |
| Overlay data RAM, tables and input work | `0x00200000`-`0x002039FF` | 14,848 including static data and two 1 KiB input buffers |
| Overlay control | `0x00207EDC`-`0x00207F07` | 44 |
| Overlay input-global pointer | `0x002081D4`-`0x002081D7` | 4 |
| Main heap, stock | allocations in 1.1 | 307,344 plus optional allocations |

**[disasm]** The known overlay-data references reach offset `0x81D7`, a 33,240-byte address span (not a claim that every byte in that span is used). The code and overlay-data areas do **not** overlap the main-RAM regions a program run from flash uses (see the hardware and firmware guide). The stock heap allocations do come from the firmware heap beginning at `0x020F6A00`, so they would directly reduce the heap a program has available.

**[inferred]** A memory-stream player need not pay the stock 300 KiB cost. Keeping the existing stream structure and decoder context costs only `0x2068` = 8,296 bytes, plus PCM storage. Four 11,025 Hz mono MPEG-2.5 buffers cost `4 * 576 * 2` = 4,608 bytes (12,904 bytes total); four 44.1 kHz mono MPEG-1 buffers cost `4 * 1152 * 2` = 9,216 bytes (17,512 bytes total). This retains the 128-descriptor structure but uses only a few descriptors. Replacing the stock pre-decode loop is necessary before shrinking the ring, because `0x060754F8` attempts 64 frames during initialisation **[disasm]**.

## Decode cost

**Method [disasm/inferred]:** the exact overlay and body instructions were run in the emulator (`sdk/hxsdk/emu.py`). File I/O and the DMA memcpy were host stubs; no instruction time was charged to them, and caches were not modelled. Counts below include header parsing, synthesis and queue bookkeeping, but exclude file I/O and audio-DMA time. Multiply/divide counts include `l.mul`, `l.muli`, `l.mulu` and the very small number of divides. The time estimate uses the camera-measured 82 M simple instructions/s and 105 ns per multiply/divide **[tested]**, from an early benchmark run:

```text
seconds = (instructions - multiply_divide) / 82,000,000
          + multiply_divide * 105e-9
```

| Stream | Instructions/frame | Multiply/divide per frame | Frame duration | Estimated CPU **[inferred]** |
|---|---:|---:|---:|---:|
| 8,000 Hz mono, MPEG-2, silence, 576 samples | 133,459-200,490 | 10,957-16,213 | 72.0 ms | 3.7-5.1% |
| 44,100 Hz stereo, MPEG-1, real music, 192 kbit/s, 1,152 samples/channel | 476,061-667,629 | 28,186-44,131 | 26.12 ms | 34.3-44.3% |

**[disasm]** The corresponding instruction rates are about 1.85-2.78 M/s for the 8 kHz sample and 18.2-25.6 M/s for the 44.1 kHz sample. These are offline counts, not camera timings; input was the public `anars/blank-audio` 250 ms silence MP3 and a prefix of `SoundHelix-Song-1.mp3`.

**[inferred]** MPEG-2.5 at 11,025 Hz mono has one eighth of the 44.1 kHz stereo output sample count (one quarter of the rate per channel, half the channels). Scaling the real-music result gives approximately **4.3-5.5% CPU**, or roughly 2.2-2.9 ms of work per 52.2 ms frame. Expect variation with spectral content and bit rate; the silence sample is a lower bound, not a music benchmark. A 44.1 kHz mono stream should cost roughly half of the measured stereo case, still about 17-22%, which is a lot for a program that also needs the CPU for other work.

## Note on the flash region
The flash range `0xD0000`-`0xFFFFF` is not all decoder code. The mode-8 loader copies only `0xD0000`-`0xD753F` as code and `0xD7600`-`0xDA7A7` as data; the rest is not referenced by this load path and its purpose is unestablished.
