# Music for the Doom port: the options we weighed

Doom's music is a MUS-format score (derived from MIDI) that the PC version plays on an OPL2 chip. The camera has no synth hardware, only a single mono PCM stream (see [`../docs/analysis/sound.md`](../docs/analysis/sound.md)), so the
port has to produce PCM itself. These are the options we compared, with their estimated costs on the camera's roughly 82 M simple-instructions-per-second CPU, and what we chose.

## What we chose
The port uses a small, simple MUS synthesiser (`hx_music.c`): cheap (about 2% of the CPU) and needing no extra files. The alternatives below would sound closer to the original but cost far more or need extra data;
we recorded the numbers in case that matters later.

## Summary of the options
- **[disasm] The firmware MP3 decoder is reusable with a memory-backed input.** Its input context contains read and seek function pointers (`0x0000EF44`, flash `0xD6F44`), so FAT is not inherent to the decoder. The firmware's output path is the already-tested direct PCM engine, not the firmware audio task at `0x02013818`.
- **[disasm] The stock player is nevertheless a poor fit unchanged:** it allocates a 288 KiB PCM ring, pre-decodes as many as 64 frames, loads 30,016 bytes of code into SRAM and uses at least 33,240 bytes of address span in the separate overlay data RAM. A custom small queue and no LRC support reduce the main-heap requirement to roughly 13-18 KiB **[inferred]**.
- **[inferred] Pre-rendered 11,025 Hz mono MP3 is practical:** approximately 4-6% of real-time CPU for real music, by scaling the measured 44.1 kHz stereo result for both rate and channel count. At 44.1 kHz stereo the measured estimate is 34-44%; native-rate OPL emulation is not a sensible Doom option.
- **[inferred] A deliberately simplified, table-driven OPL2 is feasible but borderline:** about 450-900 instructions per 11,025 Hz output sample with all 18 operators active, or 6-12% CPU. Native-rate Nuked-OPL3 is about 1.6-2.8 times the whole CPU budget and is ruled out.
- **[inferred] IMA ADPCM remains the cheap, low-risk fallback:** about 20-35 instructions/sample (0.27-0.47% CPU) and 330,750 bytes/minute (323.0 KiB/min) at 11,025 Hz mono.
- **[inferred] Recommendation:** A simple MUS synthesiser is the right first step (and is what the port uses). For authentic-sounding music without implementing OPL, pre-render the OPL/MUS tracks to **11,025 Hz mono** and use the firmware MP3 decoder with a small custom queue; choose IMA ADPCM when simplicity and negligible CPU matter more than storage. A live table-driven OPL2 is worth attempting only if MUS authenticity justifies spending most of the remaining frame headroom.


## Budget
**[inferred]** Doom itself needs most of the CPU's time each frame, leaving only a few milliseconds for music. An 11.025 kHz mono MP3 therefore fits only as a measured, carefully scheduled addition, not as a zero-cost upgrade. The stock 64-frame pre-decode should also be avoided during gameplay **[inferred]**.

## OPL2 (YM3812) emulation

### Nuked-OPL3 approach

**[inferred]** Nuked-OPL3's `OPL3_ProcessSlot` performs feedback, full envelope calculation, phase generation and waveform generation for every operator on every native sample. Its resampler runs the native generator at 49,716 Hz and interpolates outputs. Source reviewed: [Nuked-OPL3 `opl3.c`](https://raw.githubusercontent.com/nukeykt/Nuked-OPL3/master/opl3.c), especially `OPL3_EnvelopeCalc`, `OPL3_PhaseGenerate`, `OPL3_ProcessSlot` and `OPL3_Generate4ChResampled`.

**[inferred]** An OPL2-only build has 18 operators. Allowing roughly 140-250 instructions per operator plus channel/global work gives about **2,500-4,500 instructions per native sample**. At 49,716 Hz, including 18 phase multiplies/sample at 105 ns, that is approximately **160-281% of the CPU**. Merely calling the core at 11,025 Hz would still cost about 35-62% and would abandon its native-rate timing accuracy. Nuked-OPL3 is therefore not practical here.

### Small table-driven OPL2

**[inferred]** A useful budget implementation should use the exact log-sine/exponential ROM tables but precompute phase increments and envelope parameters on register writes, inline the two operators per channel, and generate directly at 11,025 Hz. A per-active-operator budget is:

| Work | Instructions/output sample/operator |
|---|---:|
| Phase add, store and waveform index | 4-6 |
| Envelope work averaged over slow ticks | 2-8 |
| Log-sine and exponential lookups, attenuation and sign | 10-20 |
| Feedback/modulation and channel accumulation | 6-12 |
| Loop/call overhead | 3-6 |
| **Total, 18 operators** | **450-900 per output sample** |

**[inferred]** At 11,025 Hz this is 5.0-9.9 M simple instructions/s, **6.1-12.1% CPU**, and about **2.8-5.6 ms per 512-sample buffer**. Register-time precomputation removes the per-sample multiply from this estimate; vibrato/rhythm handling and mixing add some cost. This is approximately the whole remaining Doom headroom, so it is plausible but must be benchmarked with all nine channels busy. Direct 11.025 kHz generation is not cycle-authentic and needs suitable anti-aliasing/envelope-rate choices, but it is the only realistic live OPL route found here.

## IMA ADPCM

**[inferred]** A conventional IMA decoder needs one index-table lookup, step-size update/clamp, three conditional difference terms, predictor update/clamp, nibble handling and output storage. That is about **20-35 instructions per 4-bit sample** on this instruction set, with no multiply required:

- 11,025 Hz: 220,500-385,875 instructions/s = **0.27-0.47% CPU**.
- 512 samples: about **0.125-0.219 ms**, before the small shared-mixer cost.
- Size: `11025 samples/s * 4 bits / 8 * 60` = **330,750 bytes/minute**, **323.0 KiB/min** or **0.3154 MiB/min**, plus negligible headers/block padding.

**[inferred]** ADPCM is four times smaller than PCM16 and dramatically cheaper to decode than either MP3 or live OPL. It cannot create authentic OPL timbre by itself, but it is the simplest way to play an authentic pre-rendered track. For comparison, 32/48/64 kbit/s MP3 takes 240,000/360,000/480,000 bytes per minute respectively **[inferred]**, so a low-bit-rate mono MP3 can be smaller than ADPCM at the cost of more CPU and integration work.


The firmware's own MP3 decoder is described in [`../docs/analysis/mp3-decoder.md`](../docs/analysis/mp3-decoder.md).
