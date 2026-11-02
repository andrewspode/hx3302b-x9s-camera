# Games (read from the disassembly)

Mode 7 'C_Game' (enter `0x0604A58C`) opens the game menu. The menu table at `0x060BDB04` has 5 entries of 40 bytes
`{u32 selected image, u32 unselected image, char name[32]}` (image = resource number in the directory at `0x100600`):

| # | Name | Images (resources) | Code (flash, in the big body, runs in place at `0x06000000 + flash`) | Debug strings |
|---|---|---|---|---|
| 0 | snake | 10 / 9 | `0x50B84`-~`0x51700` | `SnakeOpenWin`, `food.X`, `NewShape.A/B` |
| 1 | PushBox | 13 / 12 | `0x4E3DC`-`0x50B80` | `pushbox_draw_map`, `game pass,next`, levels |
| 2 | Maze | 18 / 17 | `0x4D9D4`-`0x4E3D8` | `MazeOpenWin`, `--maze level[%d] pass` |
| 3 | fly | 55 / 54 | `0x4CAD8`-`0x4D9D0` | `gFighterOpenWin`, `create_enemy` |
| 4 | 2048 | 59 / 58 | `0x4AC00`-`0x4CAD0` | `g2048OpenWin`, `g2048_draw_map`, `game over` |

Menu code: `0x4A58C`-~`0x4AC00`. All five games in the code are in the menu; no unfinished or hidden game found (unlike Sprite_tm's camera).
The games draw with JPEG resources (backgrounds, sprites) and the window system; game-over/next-level text comes from the UI strings (181 'You win. Next level', 182 'Game over').

## Modifying them
Changes inside existing code and data go in through the SD update (see [how firmware updates work](../updating-the-firmware.md)). Text strings and one game's code slot have been changed this way; other kinds of change,
such as level data, speeds, scores and images (a replacement JPEG should be the same size or smaller), should work the same way but have not been tried individually. New code needs space (unused flash
exists, for example zero padding) and a compiler for this little-endian CPU: see [`toolchain/`](../../toolchain/README.md) and [`sdk/`](../../sdk/README.md).
