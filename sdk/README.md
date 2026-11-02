# SDK: running your own programs on the camera

The stock camera cannot load programs from the SD card. This is our own SDK (the manufacturer has no public one that we know of), and it is what we added to make that possible: a small loader (the **game host**) that takes the place
of the stock "Fly" mini-game, plus `hxsdk`, the kit for writing programs the host runs. Everything here is our own design and software, not stock firmware; what we know
about the stock device is in [`../docs/hardware-and-firmware-guide.md`](../docs/hardware-and-firmware-guide.md). You need the compiler first: see [`../toolchain/`](../toolchain).

Doom was built with this, but it isn't specific to Doom: any program you can compile with the toolchain can be packaged as `GAME.BIN` and run from the SD card, or, for something
large, placed in flash. Think of it as the layer between the compiler and whatever you want to run. The limits to plan around are the hardware's: up to about 120 million simple instructions per
second, 2 MiB of RAM (roughly 889 KiB of it free for a program), and a 320x240 8-bit display layer with a palette. Beyond Doom we have only run small test programs (display, keys,
sound, memory and speed tests), so expect to find things we haven't.

| Folder | What |
|---|---|
| `loader/` | the game host (`host.c`) and its early one-shot loaders and test programs |
| `hxsdk/` | start-up code (`crt0`), linker script, the service table (`services.h`), `mkgame.py`, `build.sh`, a sound test program, a palette-chart example program (`palettechart.c`), a cache-size benchmark (`cachebench.c`), and the emulator (`emu.py`) |

## How it hooks into the stock firmware

- **[disasm]** The host occupies fly's slot at flash `0x4CBA0`, runtime `0x0604CBA0`, and must end by `0x4D94C`. Fly's event table is flash `0x13134`, copied to RAM `0x02010334`; its window descriptor is `0x02010324`.
- **[tested]** The Games-button patch changes flash `0x4A628` to open fly/host window `0x02010324` instead of game-menu window `0x020101D8`. Flash `0x4A608` can be NOPed to suppress the slide transition. One frame of the old game list was still observed. The other four games stay in flash untouched; with the menu bypassed they can no longer be reached.
- **[tested]** Host stop clears the 320x240 layer and restores the stock palette, avoiding a purple screen left behind by early test programs.

# Writing programs

## The program ABI

- **[tested]** Host loads SD-root `GAME.BIN` first; if absent, it tries an `HXDM` flash program at `0x062A0000`. A loaded program is entered as:

  ```c
  void *entry(const struct hx_services *s, u32 cmd, u32 keys, void *ctx);
  ```

  `HX_INIT` (0): return nonzero context to run, zero to refuse. `HX_FRAME` (1): return context/nonzero to continue, zero to stop. `HX_QUIT` (2): release resources; return value ignored.
- **[tested]** Services magic is `HXSV` (`0x48585356`), current version 2. In structure order: `present`, `now_ms`, `wdt_kick`, `memcpy`, `malloc`, `free`, pointers to display buffer/width/height, then `set_palette`, `f_open`, `f_read`, `f_write`, `f_close`, `f_size`. Width/height are the layer's 320x240, not the panel's 240x320.
- **[tested]** Key bits: OK/shutter 0, up 1, down 2, left 3, right 4 (as bit numbers, i.e. masks `1<<n`); `HX_KEY_ESC=1<<5`, `HX_KEY_TAP=1<<6`. Latching ensures a short press remains visible for one frame even if press and release arrived between frames.
- **[tested]** INIT is delayed until the first frame event because Games allocates its screen buffer after window open. The host then polls the key device directly, calls frames continuously and kicks the watchdog. It no longer pays the old firmware-main-loop cost of about 38 ms/frame.
- **[tested]** When the program returns zero (Doom's Quit does), host cleanup runs and mode 1 powers the camera off. There is no normal “return to Games and keep running” path in the current own-loop host.
- **[tested]** Power key in the host: the key driver decides tap against hold itself. A tap arrives as `0x19` press and release (20 ms apart) when the button is let go, and the host turns that into `HX_KEY_TAP`. A hold gives nothing at first, then `0x1A` press once the driver is sure (about 0.5-1 s), repeats every 20 ms and an explicit release; the host turns the press into `HX_KEY_ESC`. The host has no hold-to-power-off, so a program must provide its own way to power off (Doom's Quit does). (An earlier host that timed the hold itself was powered off by the driver's `0x1A` before its own timer fired.)

### The Fly handlers that were replaced
The Fly game's window (event table at flash `0x13134`, RAM `0x02010334`) already routes events into the code range the host takes over (flash `0x4CAD8`-`0x4D9D0`):

| Event | Original handler | What the host does with it |
|---|---|---|
| `0x09` open | `0x0604D080` | loads and starts the program (initialises it on the first frame event) |
| `0x0A` close | `0x0604CB38` | stops the program, then chains to the original |
| `0x0B` child close | `0x0604CACC` | stops the program, then chains to the original |
| `0x14` OK | `0x0604CBA0` | key to the program |
| `0x15`-`0x18` directions | `0x0604D910`, `D8D4`, `D898`, `D85C` | keys to the program |
| `0x19` back | `0x0604CAF8` | key to the program (the power-button tap) |
| `0x11` timer | `0x0604D69C` | relabelled `0x0D`: the host's private frame event |

## Why the host reads the keys itself
There are two ways for a program to get key presses, and the host uses the second.

1. **Firmware events.** The firmware's UI polls the key device, turns each press, repeat and release into a message, and posts it to the window that is open. This works for all
   five keys (shutter, up, down, left, right) plus the power button, but only while the firmware's own main loop is running the windows. An early version of the host worked this way, re-posting a
   private event (`0x0D`) to keep a program running frame by frame.
2. **Reading the key device directly**: the same `device(3, 0x400, &event)` call the firmware's UI poll uses. No event queue is involved.

We moved to the second for **speed**, not because the first hides any keys. The firmware's own main loop costs about 38 ms per frame, which on its own limits a program to a
few tens of frames per second (Doom took about 42 ms of its own, so it ran at around 12 fps that way). The host now runs its own loop once a program starts: it polls the key
device, calls the program, and kicks the watchdog, which is what takes Doom to 26-32 fps. Reading the raw stream also showed us how the power button really behaves (a tap is
`0x19`, a hold is `0x1A`; see [`../docs/analysis/key-scan.md`](../docs/analysis/key-scan.md)). The camera reports one key at a time either way.

## Relocatable `GAME.BIN`

- **[tested]** Programs link at address 0 with `hx.ld`, keeping relocations. `mkgame.py` emits a contiguous image including zeroed BSS, followed by 8-byte relocation records. The entry branches over this header:

| Offset | Field |
|---:|---|
| 0 | Initial `l.j` |
| 4 | `RELO` magic |
| 8 | Relocation-table offset |
| 12 | Relocation count |
| 16 | Done flag |
| 20 | Image size including BSS |
| 24 | BSS start |
| 28 | BSS size |

- **[tested]** Each table record is `{u32 offset | type<<25 | absolute<<31, u32 V}`. Supported types: `R_OR1K_32` 1, `LO_16_IN_INSN` 4, `HI_16_IN_INSN` 5, `AHI16` 35, `SLO16` 39, and `INSN_REL_26` 6 only to fixed absolute firmware addresses. Unsupported relocations fail the build.
- **[tested]** On the first call, crt0 derives its base without using relocated data, applies relocations, zeroes BSS, sets done, writes back the whole image with `0x02004524`, resets icache with `0x060691A8`, then calls `hx_main`. Later calls skip relocation. Globals, strings, function pointers, BSS and fixed firmware calls all passed on camera.
- **[tested]** Build a program named `NAME` from `sdk/hxsdk/NAME.c` with:

  ```sh
  sdk/hxsdk/build.sh NAME
  # output: out/hxsdk/NAME.GAME.BIN; copy as SD-root GAME.BIN
  ```

  The build verifies relocations offline at two alternative bases, including one whose low half crosses the `0x8000` boundary.
- **[tested]** A minimal frame loop can write palette indices to `*s->display_buffer` using `*s->width`/`*s->height`, call `s->present()`, and return its context. Use `s->wdt_kick()` in long loops. No libc is required for basic hxsdk programs.
- **[disasm]** `HX_DEBUG=1 sdk/hxsdk/build.sh NAME` adds stage records to `CRT0.LOG`. `sdk/hxsdk/emu.py` emulates INIT/FRAME/QUIT and firmware stubs, but does not model real cache/DMA hazards; camera validation remains necessary.

## Fixed-address flash programs (`HXDM`)

- **[tested]** At `0x062A0000`, an `HXDM` version-1 header contains 14 dwords: magic, version, entry, two `{dst,src,len}` copy records, two `{dst,len}` zero records, and `flash_end`. The host validates ranges/alignment, CPU-copies flash to fixed RAM, zeroes BSS, writes back, resets icache and enters `entry` using the same ABI.
- **[tested]** Host's accepted copy/zero destination window starts at `0x02011000` and ends at 2 MiB RAM, with overflow checks. Source data must lie within the declared flash image. This is a host sanity check, not proof that every destination is safe firmware-wise.
- **[tested]** Running a program from flash lets a large program keep cold code in flash while reclaiming most of the firmware heap for its zone. It is used by the Doom port; it is not the same file format as relocatable `GAME.BIN`.

