# Sound: WAV resources and a direct PCM playback recipe

**Status:** read from the disassembly and from the firmware's WAV resources, and **tested on the camera** with a small sound-test program (`sdk/hxsdk/soundtest.c`)
and the Doom port's sound effects and music. Where an early section says "untested", the last section (tested behaviour) wins: in particular, the callback
must stop the engine on bit 4, and chaining buffers from the callback on bit 2 gives gapless playback.

## Short answer

The useful entry point for our own programs is:

```c
int fn_06027804(u32 sample_rate, const void *pcm, u32 byte_count);
```

It plays a buffer of **mono, little-endian, signed 16-bit PCM** through the firmware's audio DMA engine. It returns 1 for a supported rate and 0 for an unsupported one. Supported rates are 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, and 48000 Hz.

Before calling it, write the PCM buffer back from the data cache with `0x02004524(pcm, byte_count)`. Keep the buffer alive until the audio callback reports event bit 4. Use `0x06027860()` to stop and `0x06027888(0..100)` for volume.

The higher-level `0x060385AC(resource_id, wait, volume)` is convenient for the eight stock WAV resources, but the direct call is the better interface for a custom program samples.

## WAV resources

The resource directory is at flash `0x100600`; entries 0-7 are RIFF/WAVE files. All eight have PCM format 1, one channel, 16 bits per sample, and a 44-byte header before their `data` chunk. These facts were parsed from the dump.

| ID | Flash address | Rate | PCM data bytes | Frames | Approx. duration |
|---|---|---:|---:|---:|---:|
| 0 | `0x100808` | 16000 | 13810 | 6905 | 0.432 s |
| 1 | `0x103E66` | 11025 | 26538 | 13269 | 1.204 s |
| 2 | `0x10A678` | 11025 | 12498 | 6249 | 0.567 s |
| 3 | `0x10D7B4` | 32000 | 6694 | 3347 | 0.105 s |
| 4 | `0x10F266` | 11025 | 84414 | 42207 | 3.828 s |
| 5 | `0x123C92` | 22050 | 8170 | 4085 | 0.185 s |
| 6 | `0x125CDE` | 44100 | 20000 | 10000 | 0.227 s |
| 7 | `0x12AB5E` | 11025 | 21688 | 10844 | 0.984 s |

Resource 3 is the key beep. During system initialisation, `0x06040BC0` calls `0x060386F4(1, 3, current_volume)`, which loads and prepares it. `fn_06048790()` then reaches `0x06038828`, which updates the volume and starts the prepared beep through `0x0602FB94`.

The sounds do **not** use `0x0603817C`, the resource helper already used for the palette. The WAV path instead uses:

- `0x0601F98C(id)`: locate a resource and return its resource-area address;
- `0x0601FA08(id)`: return its size;
- `0x0601FA88(address, destination, size)`: copy it into RAM.

`0x060385AC(id, wait, volume)` uses those functions to load a complete WAV, parses it through the audio-driver setup, and reaches the same direct-PCM branch. A nonzero `wait` makes it poll until playback leaves the active state; zero starts playback and returns. It returns 0 on success and -1 on failure. `0x06038554()` stops playback and frees its WAV allocation.

## Playback layers

### High-level firmware path

The direct-memory case in `0x0602FB94(descriptor)` uses these descriptor fields:

| Offset | Meaning |
|---|---|
| `+0x08` | format, 4 for this PCM path |
| `+0x0C` | source type, 2 for a memory buffer |
| `+0x1C` | volume byte |
| `+0x20` | sample rate |
| `+0x28` | PCM buffer pointer |
| `+0x2C` | PCM byte count |

It installs `0x0602FDD0` as the event callback, sets volume, and calls:

```text
0x06027804(descriptor+0x20, descriptor+0x28, descriptor+0x2C)
```

Using the firmware descriptors and audio task is possible but unnecessary for a custom program and involves shared globals at `0x02013818` and `0x02014030`/`0x02014060`.

### Low-level start function

`0x06027804(rate, buffer, bytes)` first calls `0x0602777C(rate)`. That searches an 11-entry table at `0x060A91FC`:

```text
48000, 44100, 32000, 0, 24000, 22050, 16000, 0, 12000, 11025, 8000
```

For a supported rate it calls `0x0605F19C(rate_index)` to select the rate in the audio codec, applies the current volume, and calls the DMA-start leaf at **`0x02002B78(buffer, bytes)`**. An unsupported rate returns 0 without starting DMA.

The DMA leaf performs the following hardware setup, confirmed in raw disassembly because Ghidra omits SPR accesses:

- clears bit 8 of SPR `0x8870`;
- writes `0x202C` to SPR `0x9140` and 7 to `0x914C`;
- writes the buffer address to SPR `0x9148`;
- writes `(bytes >> 2) - 1` to SPR `0x9144`;
- writes `bytes >> 1` to SPR `0x9158`;
- sets bit 3 of `0x914C` and enables interrupt 6 through `0x0600610C`.

Thus the hardware receives the original buffer, not a copy. Use a nonzero length and preferably a multiple of four bytes; the word count is truncated by the shift. The 16-bit sample count and the WAV format establish mono signed-PCM16 little-endian data.

## Volume, stop, and completion

### Volume

`0x06027888(volume)` accepts 0-100, clamps values above 100, and programs the codec through `0x0605F290`. The menu helper `0x06048E80()` maps the system's 0-15 setting to 0-80.

### Stop

`0x06027860()` sets volume zero and calls `0x0605F32C`, which writes 7 to `0x914C` and disables interrupt 6. This is the firmware's low-level stop routine. It does not clear the buffer pointer, and whether an in-flight DMA transfer drains or stops immediately has **not been tested**.

`0x0602FA54()` is the broader audio-task stop: it calls the low-level stop, clears the callback, and releases task resources. Do not use it as a substitute for normal buffer lifetime management in our own code unless we also intend to tear down the firmware's audio task.

### Event callback

Register a callback with:

```c
void fn_060278E8(void (*callback)(u32 event_bits));
```

The audio ISR at `0x06005864` reads status SPR `0x9150`, acknowledges bits through `0x914C`, and forwards the bit to the registered handler. Bit meanings established from their uses are:

| Bit | Use in the firmware |
|---|---|
| 1 | streaming buffer refill/bookkeeping |
| 2 | streaming buffer refill/bookkeeping |
| 4 | playback complete; the direct-PCM callback stops on this event |

The callback runs in interrupt context. It should only set a flag or perform other short, interrupt-safe work; do not mix audio or allocate memory there. Bit 4 is the appropriate one-shot "buffer finished" indication. `0x06030064()` only returns the firmware audio task's state byte and is not a reliable completion indicator for a direct call that bypasses that task.

The firmware initialises the codec and interrupt trampoline once through `0x06027718`, called from `0x0602E098`. Our game code should not call that initialiser again: it resets the codec, volume, and callback. The user callback pointer is at `0x02012D8C` (set by `0x060278E8`); save and restore it if the firmware's audio task may be used again later. (The ISR does not call it directly: see the callback layers in the last section.) Only one user should drive this audio engine at a time.

## Minimal custom-buffer recipe

This recipe was derived from the disassembly; the last section lists what testing changed (stop on bit 4, set the volume before every start):

```c
typedef unsigned int u32;

extern void fw_dcache_writeback(void *address, u32 length); /* 0x02004524 */
extern void fw_audio_callback(void (*callback)(u32 event)); /* 0x060278E8 */
extern void fw_audio_volume(u32 volume);                    /* 0x06027888 */
extern int  fw_audio_start(u32 rate, const void *pcm, u32 bytes); /* 0x06027804 */
extern void fw_audio_stop(void);                            /* 0x06027860 */

static volatile u32 audio_events;

static void audio_event(u32 bits)
{
    audio_events |= bits;
}

int play_pcm16(const void *samples, u32 bytes, u32 rate)
{
    if (samples == 0 || bytes == 0 || (bytes & 3) != 0)
        return 0;

    fw_audio_stop();
    audio_events = 0;
    fw_audio_callback(audio_event);
    fw_audio_volume(80);
    fw_dcache_writeback((void *)samples, bytes);
    return fw_audio_start(rate, samples, bytes);
}

int playback_finished(void)
{
    return (audio_events & 4) != 0;
}

void stop_playback(void)
{
    fw_audio_stop();
    fw_audio_callback(0);
}
```

Requirements:

- Keep `samples` valid and unchanged until `playback_finished()` becomes true or `stop_playback()` returns.
- Use main RAM that the audio DMA can access. The stock path plays from allocated main RAM; other address ranges are unvalidated.
- Use one of the nine exact rates listed above.
- Write back every newly filled buffer before restarting DMA. The start function itself does not perform this cache maintenance.
- Do not call the key-beep/resource player concurrently. Stop that activity first, and restore the old callback if returning control to firmware audio.

## Tested behaviour (`sdk/hxsdk/soundtest.c`)
- `0x060385AC(3, 0, 80)` plays the key beep (returns 0 at once). Do not use its wait mode inside a program's own loop (it polls the firmware
  audio task).
- Direct playback `0x06027804(11025, pcm16, bytes)` works: a 1 s 440 Hz tone played at the right pitch and took 999 ms. **The callback
  must stop the engine (`0x06027860()`) on bit 4**, as the firmware's own callback `0x0602FDD0` does (via `0x0602FA54`): a callback that only set a flag
  made the camera reboot about a second after the tone (most likely an interrupt storm starving the watchdog). The stop sets the volume
  to 0: call `0x06027888(v)` before every start.
- The audio ISR (`0x06005864`, runs from RAM `0x02002A64`) handles one status bit per interrupt (1, then 2, then 4), acknowledges it in SPR
  `0x914C` and calls the pointer at `0x02019784`. **Two layers:** audio init (`0x06027718`) sets `0x02019784` to a RAM dispatcher at `0x020005EC` (flash `0x33EC`),
  which loads the user callback from `0x02012D8C` and calls it with the event bits (`r3 & 0xff`). `0x060278E8(cb)` writes `0x02012D8C`. This is why
  `soundtest.c` and the Doom port install their callbacks through `0x060278E8` / `0x02012D8C` while the ISR itself reads `0x02019784`; both addresses are right.
- **Chaining works:** `0x02002B38(address, bytes)` (RAM copy of flash `0x5938`) sets the next DMA address (SPR `0x9148`) and count
  (`0x9158`). Queued right after the start, the engine plays both buffers back to back (999 ms, events `1 2 4` at the end). Queued from the
  callback on bit 2, four 250 ms buffers played as one continuous second with a single bit-4 event (`2@250 1@500 2@500 1@749 2@749 1@999
  2@999 4@999`). So: **bit 2 = the current buffer ran out and the engine moved on (queue the next one now); bit 1 = a queued buffer finished;
  bit 4 = nothing left, the engine stopped.** The chained tones are heard as continuous; restarting with `0x06027804` for each buffer
  gives a slight click (probably the codec rate re-select).
- 8-bit unsigned 11025 Hz samples convert to PCM16 and play correctly (a 5,660-sample effect took 513 ms).
- A workable design for continuous sound: a ring of small PCM16 buffers that never stops, the next one queued from the callback on bit 2 and filled by a software mixer, with silence when nothing plays
  (add +-1 dither if the amplifier turns out to gate digital silence). The Doom port does exactly this: see [`../../doom/NOTES.md`](../../doom/NOTES.md).
