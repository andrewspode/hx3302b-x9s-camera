# Key scanning and the multi-key question

**Status:** the control flow and hardware accesses below are read from the disassembly, and the event stream was **measured on the camera** with key-logging programs (event IDs, press/repeat/release, timing, and the power-button behaviour below). Runtime configuration values are from a 64 KiB SRAM dump. No key-*combination* tests have been run.

## Short answer

The shutter and four direction keys are read through **ADC channel 3**, not as independent GPIO bits. The raw read is:

```c
int sample = ((int (*)(unsigned int))0x020005B0)(3);  /* 10-bit ADC result, 0..1023 */
```

The firmware turns that one analog sample into **one** key ID, then generates press/repeat/release events. It has no simultaneous-key bitmask. A particular pair of keys might produce a distinct voltage that we could calibrate, but the raw interface cannot identify arbitrary combinations on its own. Some pairs may alias another key or combination; only measurements can determine which combinations are usable.

## Event production path

Device table entry 3 is named `key` and is located at flash `0x114E0`. Its two function pointers are:

- init: `0x0601ECA4`;
- command/read: `0x0601EEDC`.

The generic device dispatcher is:

```c
int fn_0601450C(int device_id, u32 command, void *argument);
```

Thus the existing key event interface is:

```c
u32 event = 0;
((int (*)(int, u32, void *))0x0601450C)(3, 0x400, &event);
```

`0x0601EEDC` contains the key state machine. It returns the packed event through `event`, using the representation shown below:

```text
low 16 bits:  key ID (0x14-0x1A)
high 16 bits: 0 = press, 1 = held repeat, 2 = release
```

The UI poll at `0x060493EC` issues this device-3 `0x400` request and posts the resulting packed event to the UI queue through `0x0602F36C`. The state machine holds a current/previous key, not a set of simultaneously pressed keys. The single-key limitation therefore exists in the firmware itself; a program cannot recover simultaneous keys that the firmware never reported.

For reference, the key IDs are:

| ID | Key |
|---|---|
| `0x14` | shutter / OK |
| `0x15` | up |
| `0x16` | down |
| `0x17` | left |
| `0x18` | right |
| `0x19` | "back" in the firmware: on this camera, a **short tap of the power button** |
| `0x1A` | power **held** |

**The camera has no back button.** The firmware's key set has a "back" event (`0x19`) that its menus and games use to go back or close, but on this hardware the only thing that produces it is the power button:

- **Tap:** the key driver sends `0x19` as a press and release (20 ms apart) when the button is let go.
- **Hold:** nothing at first; then, once the driver is sure it's a hold (about 0.5-1 s), a `0x1A` press, a repeat every 20 ms, and an explicit `0x1A` release.

Note the contrast with the other five keys: shutter and the four directions report a press **immediately**, then repeat events while held (the first after about 500 ms, then every 130 ms), then a release, so a program always knows whether one is down. Only the power button makes you wait: a tap is reported on release, and a hold only once the driver has decided.

That is why a tap on the power button acts as "back" in the menus, and why a program can't tell a tap from the start of a hold until the button is released or the driver decides. Power and "back" use the separate digital-input path in the key driver. The ADC discussion below concerns `0x14-0x18`.

## Raw ADC read

### Callable address and argument

The ADC wrapper is **`0x020005B0(channel)`** (flash offset `0x33B0`, running from the RAM copy). The key configuration byte at SRAM `0x1075` is 3 on this camera, and the key routines pass that value to this wrapper.

A C declaration for our programs is:

```c
extern int raw_adc_read(unsigned int channel); /* address 0x020005B0 */

int key_adc_sample(void)
{
    return raw_adc_read(3);
}
```

The wrapper selects the ADC channel, starts a conversion, waits for completion, and returns SPR `0x9178` masked with `0x3FF`. It therefore returns one unsigned 10-bit sample. It performs no key decoding and no cache operations. It can block while the ADC conversion completes, so call it from the normal game loop, not from an interrupt callback.

### How the firmware maps it to keys

`0x0601EDF8` is a smaller "current key" reader. It reads the configured ADC channel through `0x020005B0` and compares the sample against the runtime threshold table at `0x02019C58`/`0x02019C50`, returning one key ID. It does not return a raw bitmask or a list of keys.

The larger `0x0601EEDC` routine repeats this process, adds debounce/repeat handling, and creates the packed events above. Its raw ADC value is 16 bits before comparison, but the ADC wrapper itself supplies only ten significant bits.

The runtime key configuration in the SRAM dump includes:

```text
0x1074 = 1     key device present
0x1075 = 3     ADC channel
0x1076 = 0
0x1077 = 1     separate digital-key path present
```

The key table source at SRAM `0x10F8` contains the five ADC key IDs in threshold order:

```text
0x14, 0x16, 0x15, 0x17, 0x18
```

This is further evidence that all five are voltage ranges on one analog input rather than five independent digital lines.

## Can raw readings identify two keys?

**Not as a general bitmask decode.** One ADC sample is only one scalar voltage, and the firmware's table maps that voltage to one key. However, this does not rule out detecting selected combinations:

- A resistor-ladder pair can produce a voltage different from either key alone.
- If that voltage is sufficiently separated from all single-key and other-pair ranges, we can add a custom combination range.
- If it overlaps another range, that pair cannot be distinguished reliably from the other key/combination.

The practical next experiment is to log `0x020005B0(3)` at frame rate while recording:

1. no key;
2. each of `0x14-0x18` separately;
3. each direction pair, especially up+left, up+right, down+left, and down+right;
4. shutter plus each direction, if desired.

Only accept a combination whose raw range is stable and disjoint from the others. A custom driver would then map those ranges to a bitmask and generate its own press/release state. The existing `0x0601EEDC` state machine cannot represent that information.

## Which API to use

- For the **existing firmware events**, call `0x0601450C(3, 0x400, &event)`. This is the call the firmware's own UI poll makes; its results reach a window as messages, but only while the firmware's
  main loop is running the windows, and that loop costs about **38 ms per frame** on its own.
- **For speed, skip the events and call the key device yourself.** A program that runs its own loop can call `0x0601450C(3, 0x400, &event)` directly each frame, with no queue and no firmware main loop
  in the way. We did this to roughly double the frame rate of a demanding program (about 12 to 26-32 fps); see [`../../sdk/README.md`](../../sdk/README.md). The events you get are the same either way.
- For **raw combination research**, call `0x020005B0(3)` and log the integer samples.
- `0x0601EDF8` is useful as a disassembly reference for the firmware's single-key decoder, but it is not the desired multi-key API.

No combination ranges or custom multi-key behaviour have been tested.
