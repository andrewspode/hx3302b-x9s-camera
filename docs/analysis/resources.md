# Resources: sounds, pictures, font and UI strings

Read from the dump, and confirmed by rendering the strings with the camera's own font (`tools/render_strings.py`). Editing a string in place works: see "Editing a string" below.

## Directory (flash `0x100600`)
65 entries of `{u32 offset, u32 size}`, where the offset counts from `0x100600`. The directory ends at `0x100808`, where the first resource starts.
(`0x100000`-`0x1005FF` looks like OpenRISC code and has not been analysed.)

| # | Flash | Size | What |
|---|---|---|---|
| 0-7 | `0x100808`-`0x12FFFF` | | 8 `RIFF/WAVE` sounds (INFO names like `R_ID_MUSIC_3660`) |
| 8-59 | `0x130076`-`0x24D8E1` | | 52 JPEGs: full-screen backgrounds (`Welcome`, `Bye Bye`, main menu, empty menu panel), photo frames, game art |
| 60 | `0x24D8E2` | 8 | ASCII `20240528` (a date; the build or resource date?) |
| 61 | `0x24D8EA` | `0xF560` | **Font**: u32 glyph count (885), u32 start of bitmap data (relative), then `{u16 width, u16 height (24), u32 offset}` per glyph; 1 bpp, MSB first, `ceil(w/8)` bytes per row, 24 rows. Glyphs cover Latin with accents, Cyrillic, Greek?, Arabic, Hebrew, Thai, CJK and Hangul, but only the characters the strings use. |
| 62 | `0x25CE4A` | `0x12CE2` | **UI strings** (`FON` + count 18): `{u32 id 0xB700+n, u32 offset}` per language, then per language: 8-byte header, 183 entries `{u16 pixel width, u16 24, u16 length, u16 offset}`, then the text as u16 glyph codes (indices into #61). Pixel width = sum of glyph widths (exact for every string except Thai, whose combining marks carry flag bits `0x8000`/`0xC000`). |
| 63 | `0x26FB2C` | `0x1FB40` | Icons: u32 count 220, then `{offset, w, h}` (mostly 32x32) |
| 64 | `0x28F66C` | `0x400` | 256 x 4 bytes, a palette: the display layer's palette is set from 256-entry alpha5|RGB565 tables like this one (setting and restoring it works; see the display section of the [hardware and firmware guide](../hardware-and-firmware-guide.md)) |

Language names (strings 0-18, as rendered): English, Simplified Chinese, Traditional Chinese, Japanese, Korean, Russian, Turkish, Thai, Czech, Spanish, Ukrainian, German, French, Italian, Portuguese, Dutch, Hebrew, Polish, Arabic. That is 19 names for 18 blocks; block 0 is English (checked), the mapping of the other blocks is not checked.

## Some English strings (index: text)
All UI strings are stored the same way (glyph codes in resource 62); these are a few that stand out when browsing the table:

- 19 `Upgrade, please wait......` and 20 `Upgrade is complete, reboot!` are **never shown**: no code refers to them, and the only references are table entries at flash `0x11DE8`
  and `0x11DF4`, probably belonging to the "SD Update" mode that nothing ever enters (not verified). A real update shows only a progress bar and "bye bye".
- 26 `NO DC, will power off in 10s` (sic) is used by code.
- 57 `Reset System`, 159 `Capture Timer`, **160 `Systerm Volume`** (typo) and 161 `Screen Bright` are Setting-menu items.
- 125-127 are card errors; 181 `You win. Next level` and 182 `Game over` are game messages.

Not every string in the table is ever displayed. Full list: render it with `tools/render_strings.py`.

## How strings are referenced
Menu tables use resource IDs: `0x81000000 | n` = string n, `0x80000000 | n` = image n (e.g. the Setting menu items at RAM `0x0200F798` = flash `0x12598`).
The text widget takes either a C string pointer or such an ID (message 7 via `fn_0603db0c`; "TEST ON" is a plain C string in rodata, which is why it was easy to change).

## Editing a string
Change the glyph codes in place, set the new length and pixel width; a shorter string fits in its old slot. Only glyphs present in the font can be used.
Dropping the `r` glyph and shrinking the string length and pixel width does this for string 160 ("System Volume", 13 glyphs, 126 px).
