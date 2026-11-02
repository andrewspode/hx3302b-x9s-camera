/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* GAME.BIN layout: entry() first (the loader calls byte 0), then all other code. No data sections allowed (build.sh checks). */
SECTIONS { .text 0 : { *(.text.entry) *(.text .text.*) } }
