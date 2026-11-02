/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Andrew Spode */
typedef unsigned int u32;

extern void *fw_memcpy(void *dest, const void *src, u32 length);
extern void fw_send(const void *src, u32 length);

void entry(u32 *cmd, u32 *response)
{
    u32 source = cmd[14];
    u32 length = cmd[9];
    void *dest = (void *)response[2];

    fw_memcpy(dest, (const void *)source, length);
    fw_send(dest, length);
}
