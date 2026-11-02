/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
/* Self-relocation on the first call. Runs before any absolute address is valid, so: no globals, no string literals, no jump tables,
   Table entries (8 bytes): {u32 offset | type << 25 | abs << 31, u32 V}.
   And the load address comes from crt0.S (an l.jal, no relocated value), so it is right on every call.
   HX_DEBUG: append a stage byte to CRT0.LOG at each step (find where a crash happens; the name is built on the stack). Relocation table entries (8 bytes): {u32 offset | type << 25 | abs << 31, u32 V} (see mkgame.py)
   where V = the target's link-time value (image linked at 0); the patched value is A = V + (abs ? 0 : base). */
#include "services.h"
void *hx_main(const struct hx_services *s, u32 cmd, u32 keys, void *ctx);
typedef void (*wb_t)(void *, u32);
typedef void (*ic_t)(void);

#ifdef HX_DEBUG
void hx_stage(const struct hx_services *s, u32 n, u32 v)      /* also callable from programs in HX_DEBUG builds */
{
    char name[9]; name[0] = 'C'; name[1] = 'R'; name[2] = 'T'; name[3] = '0'; name[4] = '.'; name[5] = 'L'; name[6] = 'O'; name[7] = 'G'; name[8] = 0;
    u32 rec[2]; rec[0] = n; rec[1] = v;
    int fd = s->f_open(name, 3);                              /* append: open existing read/write, seek to end */
    if (fd < 0) fd = s->f_open(name, 0xB);
    if (fd < 0) return;
    ((int (*)(int, u32, u32, u32))0x0601D018)(fd, s->f_size(fd), 0, 0);
    s->f_write(fd, rec, 8);
    s->f_close(fd);
}
#define STAGE(n, v) hx_stage(s, n, v)
#else
#define STAGE(n, v) ((void)0)
#endif

void *__hx_crt0(const struct hx_services *s, u32 cmd, u32 keys, void *ctx, u32 base)
{
    volatile u32 *h = (volatile u32 *)(base + 4);                 /* __hx_header */
    if (h[0] != 0x4F4C4552) return 0;
    if (!h[3]) {
        STAGE(1, base);
        const u32 *t = (const u32 *)(base + h[1]);
        for (u32 i = 0; i < h[2]; i++, t += 2) {
            u32 off = t[0] & 0x01FFFFFF, type = (t[0] >> 25) & 0x3F, A = t[1] + ((t[0] >> 31) ? 0 : base);
            u32 *p = (u32 *)(base + off);
            if (type == 1)       *p = A;                                                    /* R_OR1K_32 */
            else if (type == 4)  *p = (*p & 0xFFFF0000) | (A & 0xFFFF);                     /* LO_16_IN_INSN */
            else if (type == 5)  *p = (*p & 0xFFFF0000) | (A >> 16);                        /* HI_16_IN_INSN */
            else if (type == 35) *p = (*p & 0xFFFF0000) | ((A + 0x8000) >> 16);             /* AHI16 */
            else if (type == 39) *p = (*p & ~((0x1Fu << 21) | 0x7FFu)) | (((A >> 11) & 0x1F) << 21) | (A & 0x7FF);   /* SLO16 */
            else if (type == 6)  *p = (*p & 0xFC000000) | (((A - (u32)p) >> 2) & 0x03FFFFFF); /* INSN_REL_26 to a fixed address */
        }
        STAGE(2, h[2]);
        for (u32 i = 0; i < h[6]; i++) ((u8 *)(base + h[5]))[i] = 0;   /* bss */
        h[3] = 1;
        /* data-cache WRITE-BACK of the patched image: 0x02004524 = RAM copy of flash 0x7324 (SPR 0x180C), what present() uses before the
           display reads a frame. NOT 0x060691EC: that one (SPR 0x1804) INVALIDATES (discards) lines, which threw our patches away. */
        ((wb_t)0x02004524)((void *)base, h[4]);
        STAGE(3, 0);
        ((ic_t)0x060691A8)();                                     /* instruction cache reset */
        STAGE(4, 0);
    }
    void *r = hx_main(s, cmd, keys, ctx);
    if (cmd == HX_INIT) STAGE(5, (u32)r);
    return r;
}
