/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* Firmware services handed to a program loaded from the SD card. The program is called as entry(const struct hx_services *).
   Addresses are this firmware's (dump sha256 in the dump); see sdk/README.md. */
#ifndef HX_SERVICES_H
#define HX_SERVICES_H
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define HX_SERVICES_MAGIC 0x48585356u          /* 'HXSV' */
struct hx_services {
    u32 magic, version;
    void  (*present)(void);                    /* show the display buffer (does its own cache write-back) */
    u32   (*now_ms)(void);                     /* millisecond counter */
    void  (*wdt_kick)(void);                   /* feed the watchdog in long loops */
    void *(*memcpy)(void *, const void *, u32);
    void *(*malloc)(u32);
    void  (*free)(void *);
    u8  **display_buffer;                      /* *display_buffer = the 8-bit overlay layer, stride = *width */
    u16  *width, *height;                      /* the LAYER's size (320 x 240), not the LCD panel's 240 x 320 */
    /* version >= 2 */
    void  (*set_palette)(u32 layer, const u32 *pal);   /* layer 0; entry = alpha5 << 16 | RGB565 (alpha 0x1F opaque, 0 clear) */
    int   (*f_open)(const char *name, int mode);       /* modes: 1 read, 3 read/write, 0xB create/overwrite; returns fd 0..3 */
    int   (*f_read)(int fd, void *buf, u32 len);
    int   (*f_write)(int fd, const void *buf, u32 len);
    void  (*f_close)(int fd);
    u32   (*f_size)(int fd);
};

/* Game host: GAME.BIN is called as  void *entry(const struct hx_services *, u32 cmd, u32 keys, void *ctx)
   cmd HX_INIT: return a context pointer (non-zero) to run, 0 to refuse. cmd HX_FRAME: return ctx to continue, 0 to stop.
   cmd HX_QUIT: free everything; return value ignored. Frames are requested as fast as the firmware main loop allows. */
enum { HX_INIT = 0, HX_FRAME = 1, HX_QUIT = 2 };
#define HX_KEY_OK    (1u << 0)                 /* shutter */
#define HX_KEY_UP    (1u << 1)
#define HX_KEY_DOWN  (1u << 2)
#define HX_KEY_LEFT  (1u << 3)
#define HX_KEY_RIGHT (1u << 4)
#define HX_KEY_ESC   (1u << 5)                 /* the power button held >= 400 ms (one frame) */
#define HX_KEY_TAP   (1u << 6)                 /* the power button tapped (released within 400 ms; one frame) */
#define HX_KEY_BACK  HX_KEY_TAP                /* (event 0x19 seen through the window, before the program's own loop runs) */
#endif
