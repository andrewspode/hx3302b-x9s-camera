/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* Firmware file functions at their fixed addresses (constants, so programs stay position independent). Modes: 1 read, 3 read/write,
   0xB read/write/create-always. fd 0..3. */
#ifndef HX_FWFILE_H
#define HX_FWFILE_H
#include "services.h"
typedef int  (*hx_f_open_t)(const char *, int);
typedef int  (*hx_f_write_t)(int, const void *, u32);
typedef void (*hx_f_close_t)(int);
#define HX_F_OPEN   ((hx_f_open_t)0x0601CD60)
#define HX_F_WRITE  ((hx_f_write_t)0x0601CF28)
#define HX_F_CLOSE  ((hx_f_close_t)0x0601CE38)
/* write a whole file; name must be built on the stack (no string literals in GAME.BIN programs) */
static inline int hx_write_file(const char *name, const void *data, u32 len)
{
    int fd = HX_F_OPEN(name, 0xB);
    if (fd < 0) return -1;
    int n = HX_F_WRITE(fd, data, len);
    HX_F_CLOSE(fd);
    return n;
}
#endif
