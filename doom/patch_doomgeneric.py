#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Andrew Spode
"""Copy doomgeneric (git e257a09) into OUT and apply the hx330x changes. Usage: patch_doomgeneric.py SRC_DIR OUT_DIR
Every edit asserts that the original text is present, so a different upstream version fails loudly."""
import os, shutil, sys
src, out = sys.argv[1], sys.argv[2]
if os.path.exists(out): shutil.rmtree(out)
shutil.copytree(src, out)
def edit(f, old, new):
    p = os.path.join(out, f); s = open(p).read()
    assert old in s, f"{f}: expected text not found: {old[:60]!r}"
    open(p, "w").write(s.replace(old, new, 1))
# 1. the screen buffer: 1 byte per pixel in CMAP256 mode (upstream always allocates 4); the platform repoints it at the display layer
edit("doomgeneric.c", "DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);",
     "DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(pixel_t));")
# 2. zone memory: whatever the platform hands over (no 6 MiB minimum)
edit("i_system.c", "byte *I_ZoneBase (int *size)\n{", "byte *hx_zone(int *size);\nbyte *I_ZoneBase (int *size)\n{\n    if (1) { byte *z = hx_zone(size); printf(\"zone memory: %p, %x allocated for zone\\n\", z, *size); return z; }")
# 3. zone in several separate regions (the camera has no single free block big enough): Z_AddRegion adds a free block + a permanent
#    end-marker block (tag PU_STATIC, user ZSENT) so blocks never merge across a gap; Z_CheckHeap skips the gap after a marker.
#    The main region also ends with a marker. hx_zone_extra() (platform) tests and adds the regions after Z_Init.
edit("z_zone.c", "memzone_t*\tmainzone;", """memzone_t*\tmainzone;
#define ZSENT ((void **)0x5E7)
static void Z_LinkSentinel(memblock_t *after, memblock_t *s)
{
    s->size = sizeof(memblock_t); s->tag = PU_STATIC; s->user = ZSENT; s->id = ZONEID;
    s->prev = after; s->next = after->next; after->next->prev = s; after->next = s;
}
void Z_AddRegion(byte *base, int size)
{
    memblock_t *f = (memblock_t *)base, *last = mainzone->blocklist.prev;
    size &= ~15;
    if (size < 4096) return;
    f->size = size - sizeof(memblock_t); f->tag = PU_FREE; f->user = NULL; f->id = 0;
    f->prev = last; f->next = &mainzone->blocklist; last->next = f; mainzone->blocklist.prev = f;
    Z_LinkSentinel(f, (memblock_t *)(base + f->size));
    mainzone->size += size;
}
void hx_zone_extra(void);""")
edit("z_zone.c", """    block->size = mainzone->size - sizeof(memzone_t);
}""", """    block->size = mainzone->size - sizeof(memzone_t) - sizeof(memblock_t);
    Z_LinkSentinel(block, (memblock_t *)((byte *)block + block->size));
    hx_zone_extra();
}""")
# (the check line occurs three times; match the I_Error one in Z_CheckHeap: patching Z_DumpHeap's copy instead fails on the gap after a marker)
edit("z_zone.c", """	if ( (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap""", """	if ( block->user != ZSENT && (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap""")
# 4. (debug) step logging around level loading, to find where a crash happens; HXLOG is printf to DOOM.LOG
for f, old in (("g_game.c", "void G_DoLoadLevel (void) \n{ \n    int             i; \n"),
               ("g_game.c", "void G_DoPlayDemo (void) \n{ \n    skill_t skill; \n    int             i, episode, map; \n    int demoversion;\n"),
               ("r_data.c", "void R_PrecacheLevel (void)\n{\n    char*\t\tflatpresent;\n    char*\t\ttexturepresent;\n    char*\t\tspritepresent;\n"),
               ("d_main.c", "void D_DoAdvanceDemo (void)\n{\n")):
    fn = old.split("(")[0].split()[-1]
    edit(f, old, old + f'    printf("hx330x step: {fn}\\n");\n')
edit("p_setup.c", "    int\t\ti;\n    char\tlumpname[9];\n    int\t\tlumpnum;\n",
     "    int\t\ti;\n    char\tlumpname[9];\n    int\t\tlumpnum;\n    printf(\"hx330x step: P_SetupLevel %d %d\\n\", episode, map);\n")
edit("p_setup.c", "\tR_PrecacheLevel ();", "\t{ printf(\"hx330x step: P_SetupLevel loaded, precache\\n\"); R_PrecacheLevel (); printf(\"hx330x step: precache done\\n\"); }")
edit("r_main.c", "void R_RenderPlayerView (player_t* player)\n{\t\n", "void R_RenderPlayerView (player_t* player)\n{\t\n    { static int n; if (n < 3) printf(\"hx330x step: R_RenderPlayerView %d\\n\", n++); }\n")
# 5. (test) HX_TITLE_ONLY: never leave the title page (is the crash in the demo/level, or in the firmware while Doom runs?)
edit("d_main.c", "void D_PageTicker (void)\n{\n    if (--pagetic < 0)", "void D_PageTicker (void)\n{\n#ifdef HX_TITLE_ONLY\n    if (pagetic < 100) pagetic = 100;\n#endif\n    if (--pagetic < 0)")
# 6. (removed: wipe/draw logging around present seemed to provoke crashes)
# 7. memory ('Z_Malloc: failed on allocation of 64024 bytes' at the first wipe into the demo level):
#    Doom draws straight into DG_ScreenBuffer (the display layer, set by DG_Init before I_InitGraphics) instead of a 64000-byte zone copy,
#    and the screen melt (two more 64000-byte zone buffers at every transition) is off unless HX_WIPE is defined.
edit("i_video.c", "\tI_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on",
     "\t{ extern byte *hx_video_buffer(void); I_VideoBuffer = hx_video_buffer(); }  // hx330x: the display layer itself, or a 320x200 buffer to stretch (HX_STRETCH)")
edit("i_video.c", "\tZ_Free (I_VideoBuffer);", "\t/* hx330x: I_VideoBuffer is the display layer or a static zone block for the whole run */")
edit("i_video.c", "    /* DRAW SCREEN */\n", "    if (1) { DG_DrawFrame(); return; }   /* hx330x: DG_DrawFrame shows I_VideoBuffer (in place, or stretched 200 -> 240 rows) */\n    /* DRAW SCREEN */\n")
edit("d_main.c", "    if (gamestate != wipegamestate)\n\t\t{", "#ifndef HX_WIPE\n    wipegamestate = gamestate;              /* hx330x: no melt (saves 128 KB of zone) */\n#endif\n    if (gamestate != wipegamestate)\n\t\t{")
# 8. (debug) on a zone failure, log what the zone holds: bytes per tag, free bytes, the largest free and largest free+purgeable run
#    (is the zone full of locked data, or fragmented?)
edit("z_zone.c", "void hx_zone_extra(void);", """void hx_zone_extra(void);
static void Z_HxStats(int want)
{
    int tags[PU_NUM_TAGS + 1] = {0}, nblk = 0, bigfree = 0, bigrun = 0, run = 0;
    memblock_t *b;
    for (b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
        nblk++;
        if (b->tag >= 0 && b->tag <= PU_NUM_TAGS) tags[b->tag] += b->size;
        if (b->tag == PU_FREE && b->size > bigfree) bigfree = b->size;
        if (b->tag == PU_FREE || b->tag >= PU_PURGELEVEL) { run += b->size; if (run > bigrun) bigrun = run; } else run = 0;
    }
    printf("zone stats (want %d): %d blocks; static %d sound %d music %d level %d levspec %d purgelevel %d cache %d free %d; "
           "largest free %d, largest free+purgeable run %d\\n", want, nblk, tags[PU_STATIC], tags[PU_SOUND], tags[PU_MUSIC], tags[PU_LEVEL],
           tags[PU_LEVSPEC], tags[PU_PURGELEVEL], tags[PU_CACHE], tags[PU_FREE], bigfree, bigrun);
}""")
edit("z_zone.c", "            I_Error (\"Z_Malloc: failed on allocation of %i bytes\", size);",
     "            { Z_HxStats(size); I_Error (\"Z_Malloc: failed on allocation of %i bytes\", size); }")
# 9. fragmentation (226 KB free but the largest free block 31.6 KB; small level objects pin the gaps left by purged cache):
#    allocations of up to HX_SMALL bytes start their search at their own rover, inside the extra regions (the first one added), and
#    everything else at another rover in the main region, so the small objects do not break up the space big blocks need. Z_Free keeps both
#    rovers valid when it merges blocks.
edit("z_zone.c", "#define ZSENT ((void **)0x5E7)\n", """#define ZSENT ((void **)0x5E7)
#define HX_SMALL (1024 + (int)sizeof(memblock_t))
static memblock_t *hx_rover_small, *hx_rover_big;
""")
edit("z_zone.c", "    Z_LinkSentinel(f, (memblock_t *)(base + f->size));\n", "    Z_LinkSentinel(f, (memblock_t *)(base + f->size));\n    if (!hx_rover_small) hx_rover_small = f;\n")
edit("z_zone.c", "        if (block == mainzone->rover)\n            mainzone->rover = other;\n",
     "        if (block == mainzone->rover)\n            mainzone->rover = other;\n        if (block == hx_rover_small) hx_rover_small = other;\n        if (block == hx_rover_big) hx_rover_big = other;\n")
edit("z_zone.c", "        if (other == mainzone->rover)\n            mainzone->rover = block;\n",
     "        if (other == mainzone->rover)\n            mainzone->rover = block;\n        if (other == hx_rover_small) hx_rover_small = block;\n        if (other == hx_rover_big) hx_rover_big = block;\n")
edit("z_zone.c", "    base = mainzone->rover;\n", """    int hx_small = size <= HX_SMALL && hx_rover_small;
    if (hx_small) mainzone->rover = hx_rover_small;
    else if (hx_rover_big) mainzone->rover = hx_rover_big;
    base = mainzone->rover;
""")
edit("z_zone.c", "    // next allocation will start looking here\n    mainzone->rover = base->next;",
     "    // next allocation will start looking here\n    mainzone->rover = base->next;\n    if (hx_small) hx_rover_small = base->next; else hx_rover_big = base->next;")
edit("z_zone.c", "    Z_LinkSentinel(block, (memblock_t *)((byte *)block + block->size));\n    hx_zone_extra();",
     "    Z_LinkSentinel(block, (memblock_t *)((byte *)block + block->size));\n    hx_rover_big = block;\n    hx_zone_extra();")
# 10. memory (after demo 1, 378 KB free but the largest piece 60 KB, so the 68 KB CREDIT picture failed): free the old level as
#     soon as Doom leaves it for a picture screen (demo sequence) or the intermission, instead of at the next level load. Nothing reads level
#     data in those states; P_SetupLevel frees the same tags again (a no-op then). Sound channels are stopped first, as P_SetupLevel does.
edit("s_sound.c", "void S_Start(void)\n{", "void S_HxStopAll(void)\n{\n    for (int cnum = 0; cnum < snd_channels; cnum++) if (channels[cnum].sfxinfo) S_StopChannel(cnum);\n}\n\nvoid S_Start(void)\n{")
edit("p_setup.c", "    int\t\ti;\n    char\tlumpname[9];\n    int\t\tlumpnum;\n    printf(",
     "    int\t\ti;\n    char\tlumpname[9];\n    int\t\tlumpnum;\n    hx_level_loaded = 1;\n    printf(")
edit("p_setup.c", "\nvoid\nP_SetupLevel", """
int hx_level_loaded;
void S_HxStopAll(void);
void hx_free_level(const char *why)                 /* hx330x: see patch 10 */
{
    if (!hx_level_loaded) return;
    hx_level_loaded = 0;
    S_HxStopAll();
    Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1);
    P_InitThinkers();
    printf("hx330x step: level freed (%s)\\n", why);
}

void
P_SetupLevel""")
edit("d_main.c", "    if (gameversion == exe_ultimate || gameversion == exe_final)\n      demosequence",
     "    int hx_was_level = gamestate == GS_LEVEL;\n    if (gameversion == exe_ultimate || gameversion == exe_final)\n      demosequence")
edit("d_main.c", "    switch (demosequence)\n    {\n      case 0:", "    { extern void hx_free_level(const char *); if (hx_was_level && demosequence % 2 == 0) hx_free_level(\"demo page\"); }\n    switch (demosequence)\n    {\n      case 0:")
edit("g_game.c", "    StatCopy(&wminfo);\n ", "    StatCopy(&wminfo);\n    { extern void hx_free_level(const char *); hx_free_level(\"intermission\"); }\n ")
# 11. camera controls in Doom's menus: the shutter (KEY_ENTER in menus) confirms y/n prompts, the power button's short press (Escape) cancels;
#     prompts name those buttons; Quit returns to the host (doom_main returns 0 -> the host powers the camera off) instead of exit().
#     Next weapon: a key code the platform sends on a tap of the power button.
edit("m_controls.c", "int key_menu_confirm   = 'y';", "int key_menu_confirm   = KEY_ENTER;           /* hx330x: shutter */")
edit("m_controls.c", "int key_nextweapon = 0;", "int key_nextweapon = ']';                      /* hx330x: power button tap */")
edit("m_controls.c", "int key_menu_abort     = 'n';", "int key_menu_abort     = KEY_BACKSPACE;       /* hx330x: power button tap (a hold is Escape, also 'no') */")
edit("d_englsh.h", '#define PRESSYN \t"press y or n."', '#define PRESSYN \t"fire = yes, power = no."')
edit("d_englsh.h", '#define DOSY\t\t"(press y to quit to dos.)"', '#define DOSY\t\t"(fire = switch the camera off.)"')
edit("i_system.c", "void I_Quit (void)\n{\n    atexit_listentry_t *entry;\n", "int hx_quit;\nvoid I_Quit (void)\n{\n    atexit_listentry_t *entry;\n    if (1) { hx_quit = 1; return; }    /* hx330x: the platform returns 0 to the host, which powers off */\n")
# 12. sound (FEATURE_SOUND, our DG_sound_module / DG_music_module in doom/hx_sound.c): no SDL_mixer header
edit("i_sound.c", "#if defined(FEATURE_SOUND) && !defined(__DJGPP__)\n#include <SDL_mixer.h>\n#endif", "/* hx330x: no SDL_mixer (doom/hx_sound.c) */")
# 13. (profiling) time per phase, accumulated in hx_prof[] (doom/hx_doom.c): 0 game tics, 1 3D view, 2 status bar + HUD
edit("d_net.c", "    G_Ticker ();", "    { extern unsigned hx_prof[], hx_now(void); unsigned _t = hx_now(); G_Ticker (); hx_prof[0] += hx_now() - _t; }")
edit("d_main.c", "    \tR_RenderPlayerView (&players[displayplayer]);", "    \t{ extern unsigned hx_prof[], hx_now(void); unsigned _t = hx_now(); R_RenderPlayerView (&players[displayplayer]); hx_prof[1] += hx_now() - _t; }")
edit("d_main.c", "\t\tST_Drawer (viewheight == 200, redrawsbar );", "\t\t{ extern unsigned hx_prof[], hx_now(void); unsigned _t = hx_now(); ST_Drawer (viewheight == 200, redrawsbar ); hx_prof[2] += hx_now() - _t; }")
edit("d_main.c", "    \tHU_Drawer ();", "    \t{ extern unsigned hx_prof[], hx_now(void); unsigned _t = hx_now(); HU_Drawer (); hx_prof[2] += hx_now() - _t; }")
# 14. WAD reads straight through the platform's read() (stdio's 1 KiB buffer made a lump read into ~1000 separate 1 KiB card
#     reads, ~0.8 ms each: 992 KB in 812 ms over 50 frames); read() fetches up to 16 KiB per card access.
edit("w_file_stdc.c", "    fseek(stdc_wad->fstream, offset, SEEK_SET);\n\n    // Read into the buffer.\n\n    result = fread(buffer, 1, buffer_len, stdc_wad->fstream);",
     "    { extern long lseek(int, long, int); extern int read(int, void *, unsigned); int fd = fileno(stdc_wad->fstream);   /* hx330x: no stdio buffer */\n"
     "      lseek(fd, (long)offset, SEEK_SET); int r = read(fd, buffer, (unsigned)buffer_len); result = r > 0 ? (size_t)r : 0; }")
# 15. (debug) who holds the zone's static memory: on a zone failure, the 12 biggest PU_STATIC blocks with their WAD lump name (if the block
#     is a cached lump); Z_HxStaticBytes() for the 50-frame log line (static grew 184 -> 294 KB after sound arrived)
edit("z_zone.c", "static void Z_HxStats(int want)\n{", """#include <string.h>
#include "w_wad.h"
int Z_HxStaticBytes(void)
{
    int n = 0;
    for (memblock_t *b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) if (b->tag == PU_STATIC && b->user != ZSENT) n += b->size;
    return n;
}
static void Z_HxStaticTop(void)
{
    memblock_t *top[12] = {0};
    for (memblock_t *b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
        if (b->tag != PU_STATIC || b->user == ZSENT) continue;
        for (int i = 0; i < 12; i++) if (!top[i] || b->size > top[i]->size) { for (int k = 11; k > i; k--) top[k] = top[k - 1]; top[i] = b; break; }
    }
    for (int i = 0; i < 12 && top[i]; i++) {
        void *data = (byte *)top[i] + sizeof(memblock_t);
        const char *nm = "-";
        char buf[9] = {0};
        for (unsigned l = 0; l < numlumps; l++) if (lumpinfo[l].cache == data) { memcpy(buf, lumpinfo[l].name, 8); nm = buf; break; }
        printf("  static %7d bytes at %p user %p lump %s\\n", top[i]->size, data, (void *)top[i]->user, nm);
    }
    int sounds = 0, sound_bytes = 0;
    for (memblock_t *b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
        if (b->tag != PU_STATIC || b->user == ZSENT) continue;
        void *data = (byte *)b + sizeof(memblock_t);
        for (unsigned l = 0; l < numlumps; l++)
            if (lumpinfo[l].cache == data) { if (lumpinfo[l].name[0] == 'D' && lumpinfo[l].name[1] == 'S') { sounds++; sound_bytes += b->size; } break; }
    }
    printf("  static sound lumps (DS*): %d, %d bytes\\n", sounds, sound_bytes);
}
static void Z_HxStats(int want)
{""")
edit("z_zone.c", "           tags[PU_LEVSPEC], tags[PU_PURGELEVEL], tags[PU_CACHE], tags[PU_FREE], bigfree, bigrun);\n}",
     "           tags[PU_LEVSPEC], tags[PU_PURGELEVEL], tags[PU_CACHE], tags[PU_FREE], bigfree, bigrun);\n    Z_HxStaticTop();\n}")
# 16. no music player yet: do not cache the music lump (PU_STATIC, ~10 KB pinned in the zone for nothing, seen at D_E1M5)
edit("s_sound.c", "void S_ChangeMusic(int musicnum, int looping)\n{\n    musicinfo_t *music = NULL;\n    char namebuf[9];\n    void *handle;\n",
     "void S_ChangeMusic(int musicnum, int looping)\n{\n    musicinfo_t *music = NULL;\n    char namebuf[9];\n    void *handle;\n    { extern int hx_music_ok; if (!hx_music_ok) return; }   /* hx330x: doom/hx_sound.c */\n")
# 17. a power-button tap (KEY_BACKSPACE = menu back) at the top-level menu closes the menu (a tap at the main menu otherwise did nothing)
edit("m_menu.c", "\t    currentMenu = currentMenu->prevMenu;\n\t    itemOn = currentMenu->lastOn;\n\t    S_StartSound(NULL,sfx_swtchn);\n\t}\n\treturn true;",
     "\t    currentMenu = currentMenu->prevMenu;\n\t    itemOn = currentMenu->lastOn;\n\t    S_StartSound(NULL,sfx_swtchn);\n\t}\n\telse { M_ClearMenus (); S_StartSound(NULL,sfx_swtchx); }   /* hx330x */\n\treturn true;")
# 18. a loading bar while a level loads (after the start-up bar there would be no sign of the level loading): six steps in P_SetupLevel
for old, k in (("    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);\n", 1), ("    P_LoadSideDefs (lumpnum+ML_SIDEDEFS);\n", 2), ("    P_LoadSegs (lumpnum+ML_SEGS);\n", 3),
               ("    P_GroupLines ();\n", 4), ("    P_LoadThings (lumpnum+ML_THINGS);\n", 5), ("    P_SpawnSpecials ();\n", 6)):
    new = (f"    {{ extern void hx_loading(int, int); hx_loading({k - 1}, 6); }}\n" + old) if k == 1 else (old + f"    {{ extern void hx_loading(int, int); hx_loading({k}, 6); }}\n")
    edit("p_setup.c", old, new)
# 19. save / load on the camera (saving otherwise needs typing a name): saves in the card's root as
#     doomsav0-7.dsg (8.3 names; our file layer cannot make the .savegame folder), written straight to the slot's file (no temp file:
#     there is no rename), and choosing a slot in "Save Game" saves at once as "SAVE n - ExMy" (no keyboard).
edit("d_main.c", "        savegamedir = M_GetSaveGameDir(D_SaveGameIWADName(gamemission));",
     '        savegamedir = "";                 /* hx330x: the card\'s root (patch 19) */')
edit("g_game.c", "    temp_savegame_file = P_TempSaveGameFile();\n    savegame_file = P_SaveGameFile(savegameslot);",
     "    savegame_file = P_SaveGameFile(savegameslot);\n    temp_savegame_file = savegame_file;    /* hx330x: write the slot's file directly (no rename on the camera) */")
edit("g_game.c", "    remove(savegame_file);\n    rename(temp_savegame_file, savegame_file);\n",
     "    /* hx330x: written in place, nothing to rename */\n")
edit("m_menu.c", """void M_SaveSelect(int choice)
{
    // we are going to be intercepting all chars
    saveStringEnter = 1;
""", """void M_SaveSelect(int choice)
{
    if (1) {                                  /* hx330x: no keyboard: name the save and save at once */
        saveSlot = choice;
        M_snprintf(savegamestrings[choice], SAVESTRINGSIZE, "SAVE %d - E%dM%d", choice + 1, gameepisode, gamemap);
        M_DoSave(choice);
        return;
    }
    // we are going to be intercepting all chars
    saveStringEnter = 1;
""")
# 20. in-place stretch: Doom draws into rows 0-199 of the display layer, which the platform then spreads over 240 rows, so the
#     layer never holds last frame's 320x200 picture: the two partial redraws must become full ones every frame (a 64 KB
#     buffer for an out-of-place stretch would put the zone over its limit).
edit("d_main.c", "redrawsbar = true;              // just put away the help screen\n",
     "redrawsbar = true;              // just put away the help screen\n\t\tredrawsbar = true;              /* hx330x: whole status bar every frame (in-place stretch) */\n")
edit("d_main.c", "\t\tif (borderdrawcount)\n\t\t{\n\t\t\tR_DrawViewBorder ();    // erase old menu stuff",
     "\t\tborderdrawcount = 3;            /* hx330x: whole border every frame (in-place stretch) */\n\t\tif (borderdrawcount)\n\t\t{\n\t\t\tR_DrawViewBorder ();    // erase old menu stuff")
