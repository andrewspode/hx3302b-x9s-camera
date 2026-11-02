# UI modes, events and every path into the SD update

Read from the disassembly (made with `tools/ghidra_scripts/DumpDisasm.java 06014000 0607e000`). Addresses are Ghidra addresses (flash + `0x06000000`).
The TEST ON path is confirmed on the camera, and the trigger is **proven**: a plain power-on runs the update check, and a file with a changed header time
flashes. (A very small update finishes too quickly for the progress bar to be noticed.) Everything else in this file is read from the disassembly.

## Initialised data lives in RAM at flash + 0x01FFD200
The "window tables" around flash `0x12E80`-`0x13E70` are never referenced at their flash address. The code uses RAM `0x0201xxxx` addresses that equal
flash + `0x01FFD200` (the same offset as the `func_0x02......` calls). Example: the 'Bat Charge' entry's field `0x020100E8` = flash `0x12EE8`, its event table.

## Modes (the "windows" were really modes)
`fn_060471bc` (called early in UI init `fn_06040bc0`) clears the mode table at RAM `0x02019270` and registers each mode with `fn_06047114(id, desc)`.
Descriptor = `{name, arg, enter, exit, service}` (5 dwords). The mode loop `fn_060473b4` calls them; `fn_060472e8(id, arg)` requests a switch
(fields `+0x40` current, `+0x44` requested, `+0x48` previous); `fn_060472c8()` returns the current mode.

| id | name | enter | flash of desc |
|---|---|---|---|
| 0 | Bat Charge | `0x06047514` | `0x12EC4` |
| 1 | Power Off | `0x0605793C` | `0x13C54` |
| 2 | main | `0x060526A8` | `0x1380C` |
| 3 | Record Video | `0x06059568` | `0x13D08` |
| 4 | Record Photo | `0x06057BB4` | `0x13C68` |
| 5 | Play Video | `0x06055204` | `0x13AA8` |
| 7 | C_Game | `0x0604A58C` | `0x12FA0` |
| 8 | Play mp3 | `0x060531F0` | `0x13888` |
| 9 | Setting | `0x0605B8B8` | `0x13DC8` |
| 10 | usb device | `0x0605B95C` | `0x13DDC` |
| 12 | SD Update | `0x0605B764` (= just calls `sd_update_run`) | `0x13DB4` |

**No code requests mode 12.** All ~22 calls of `fn_060472e8` were checked: constants 0,1,2,3,0xA,0xB, a menu table at `0x060BF07C`
(`{0x16→4, 0x13→3, 0x15→5, 0x14→9, 0x18→7, 0x17→8}`), and two that pass 0 or 1. The 'SD Update' mode looks like dead code (or reached in a way not found).

## Events
Window event tables are `{id, handler}` pairs ending `{0x1B, 0}`. Handlers get `(win, r4 = 1, r5 = &param)`.
Global table `0x060BD5E4` (registered at `0x060472B0`): ids `0x14`-`0x19` → `0x060480DC` (generic key: beep/backlight), so **0x14-0x19 are keys**;
`0x1A` → `0x060481C0` = **power key** (param 0: prints `[SYS] deal:power off`, switches to mode 1, except in mode 0); `2` → `0x0604812C` = USB (`[COM] usbdev in/out`; param 4 → mode 10).
**0x14 is the shutter**: confirmed by the TEST ON trial (below).

## SD card state (`0x0201A011`), poll `fn_060493ec`
States (strings from the switch at `0x060BD954`): 0 `sdc out`, 1 `sdc wait stable`, 2 `sdc in`, 3 `sdc error`, 4 `sdc full`, 5 `sdc normal`. `sd_update_run` needs state > 1.
The poll runs from UI init (`0x06040F78`) and from the mode loop's service tick (`fn_06041090`). **It skips card handling while the current mode is 0 (Bat Charge) or 1.**
On the first poll after boot (flag bit 0 of `0x020192DC` still clear) it skips the 500 ms debounce, goes straight to state 2 and mounts. Mode 0xF (none) is current at that
point, so **a card inserted at power-on is mounted during init**, before any update call below. (If the card is inserted while on the charge screen, it is not seen.)

## Every call of `sd_update_run` (`0x02001B58`)
1. **Every normal power-on** (`0x060421B8`, in UI start `fn_06042164`): after init, if `0x0201A009` bit 1 (USB power present) is clear, it calls `sd_update_run`, then enters 'main'.
   If the bit is set it enters 'Bat Charge' instead.
2. **Charge screen, power key** (`0x06047E80`, Bat Charge event 0x1A, param 0): calls `sd_update_run`, then 'main' (i.e. "turn on from the charge screen").
3. **Charge screen, shutter held** (`0x06047EE4`, event 0x14, param 1): counts repeats in `0x020192BC`; after more than 30, sets the test flag (`fn_0604a210(1)`,
   word `0x020192E8`), calls `sd_update_run`, then switches to 'Record Video'. the video window (Record Video) shows **TEST ON** when the flag is set (`0x0605B1BC`/`0x0605B70C`)
   and clears it. This is the only setter of the flag, so **seeing TEST ON after holding the shutter on the charge screen proves path 3 ran, and `sd_update_run` was called.**
4. Mode 12 'SD Update' enter (`0x0605B764`): never requested (see above).

## Conclusion
By this reading the update check runs on every power-on (not on the charger) and on leaving the charge screen. With a file identical to the installed
firmware it refuses silently, so a first no-op trial looks exactly like "nothing happened". This was later confirmed: a plain power-on with a changed-time
file flashes it.

## The decision and the early exits
Re-read of the decision (flash `0x4A90`-`0x4C30`): status 5 if file time != installed time, **or** file checksum != installed checksum. Changing the time alone is enough by this reading.
(The file seek at `0x06004B7C` really is to offset `0x10`: `l.ori r5,r20` after the `l.jal` runs after the call, since there are no delay slots; the seek takes a 64-bit offset in r4/r5.)
The write path never returns: after the loop (or a write error) it prints `OK`/the error letter, redraws, waits, calls `0x0A06E598` and spins at `0x060053E0`.
So a camera that carries on normally means one of the early exits: `no sdc` (state <= 1), open fail, status <= 0, `[SD update] clbtal err` (fast-seek table, `0x06004DC4`), `[SD UPDATE] malloc buf err` (`0x06004EC4`).
The debug prints (`0x06004480`) only come out when byte `0x00001360` is set, which a UART init near `0x060071EC` does; a UART log would name the exit.
