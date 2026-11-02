#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Andrew Spode
# Build Doom (doomgeneric + hx330x platform) as a relocatable GAME.BIN for the game host: out/doom/DOOM.GAME.BIN
# Needs: the or1kle compiler + newlib (built into out/toolchain by ../toolchain/setup.sh on first use; OR1KLE points elsewhere). doomgeneric (git e257a09) is fetched into out/deps/ on first use
# unless DOOMSRC points at an existing checkout's inner doomgeneric/ folder.
set -eu
R=$(cd "$(dirname "$0")/.." && pwd); OR1KLE=${OR1KLE:-$R/out/toolchain}; T=$OR1KLE/bin; S=$R/sdk/hxsdk; D=$R/doom; O=$R/out/doom
[ -x $T/or1kle-elf-gcc ] || sh $R/toolchain/setup.sh $OR1KLE
if [ -z "${DOOMSRC:-}" ]; then
  DOOMSRC=$R/out/deps/doomgeneric/doomgeneric
  if [ ! -d "$DOOMSRC" ]; then
    mkdir -p $R/out/deps
    git clone -q https://github.com/ozkl/doomgeneric.git $R/out/deps/doomgeneric
    git -C $R/out/deps/doomgeneric checkout -q e257a09
  fi
fi
mkdir -p $O/obj
python3 -I $D/patch_doomgeneric.py $DOOMSRC $O/src
CF="-ffile-prefix-map=$R=. ${OPT:--O2} ${DOOM_DEFS:-} -fno-pic -mhard-div -mhard-mul -ffunction-sections -fdata-sections -fno-strict-aliasing -DCMAP256 -DFEATURE_SOUND -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -D_DEFAULT_SOURCE -I$S -I$O/src"
SRC="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric"
OBJS="$O/obj/crt0.o $O/obj/crt0c.o"
$T/or1kle-elf-gcc $CF -c $S/crt0.S -o $O/obj/crt0.o
$T/or1kle-elf-gcc $CF ${HX_DEBUG:+-DHX_DEBUG} -fno-tree-loop-distribute-patterns -fno-jump-tables -ffreestanding -c $S/crt0.c -o $O/obj/crt0c.o
for f in $SRC; do $T/or1kle-elf-gcc $CF -w -c $O/src/$f.c -o $O/obj/$f.o; OBJS="$OBJS $O/obj/$f.o"; done
$T/or1kle-elf-gcc $CF -c $D/hx_stack.S -o $O/obj/hx_stack.o; OBJS="$OBJS $O/obj/hx_stack.o"
for f in hx_doom hx_syscalls hx_sound hx_music; do $T/or1kle-elf-gcc $CF -Wall -c $D/$f.c -o $O/obj/$f.o; OBJS="$OBJS $O/obj/$f.o"; done
LIBS="$OR1KLE/or1k-elf/lib/libc.a $OR1KLE/or1k-elf/lib/libm.a $($T/or1kle-elf-gcc -print-libgcc-file-name)"
$T/or1kle-elf-ld --no-warn-rwx-segments -q --gc-sections -e _start -u hx_main -T $S/hx.ld -Map $O/doom.map -o $O/doom.elf $OBJS --start-group $LIBS --end-group
python3 -I $S/mkgame.py $O/doom.elf $O/DOOM.GAME.BIN
$T/or1kle-elf-size $O/doom.elf
