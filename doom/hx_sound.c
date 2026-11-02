/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 Andrew Spode */
/* Doom sound effects on the camera (docs/analysis/sound.md, "Tested on the camera"): one never-ending stream of NB small PCM16 buffers at
   11025 Hz mono, played by the firmware's audio DMA (0x06027804) and chained from the audio callback: on bit 2 (the engine moved on to
   the queued buffer) the next filled buffer is queued with 0x02002B38, or a silent one if the mixer has fallen behind (a level load), so
   the engine never stops (stopping and restarting clicks). The mixer runs in Doom's sound update (once a frame) and keeps up to NB-2
   buffers ahead: 8 channels of Doom's own 8-bit sound lumps, volume per channel, no stereo (the speaker is mono). Music: not yet (stub). */
#include <string.h>
#include "services.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

extern const struct hx_services *hx_svc;
typedef int  (*au_start_t)(u32 rate, const void *pcm, u32 bytes);
typedef void (*au_v_t)(void);
typedef void (*au_vol_t)(u32);
typedef void (*au_cb_t)(void (*)(u32));
typedef void (*au_queue_t)(const void *, u32);
typedef void (*wb_t)(void *, u32);
#define AU_START ((au_start_t)0x06027804)
#define AU_STOP  ((au_v_t)0x06027860)
#define AU_VOL   ((au_vol_t)0x06027888)
#define AU_CB    ((au_cb_t)0x060278E8)
#define AU_QUEUE ((au_queue_t)0x02002B38)
#define FW_WB    ((wb_t)0x02004524)

#define RATE 11025
#define BS   512                                 /* samples per buffer: 46 ms */
#define NB   4
#define NCH  16                                  /* Doom asks for snd_channels (8); allow more */
#define HW_VOLUME 80

int use_libsamplerate = 0;                       /* (config variables the SDL module would define) */
float libsamplerate_scale = 0.65f;

static short buf[NB][BS] __attribute__((aligned(64)));
static short silence[BS] __attribute__((aligned(64)));
static volatile u32 q_next;                      /* index of the next buffer to queue (callback) */
static u32 filled;                               /* buffers filled so far (mixer) */
static volatile int running, stopped, underruns;

static void au_event(u32 bits)                   /* interrupt context: queue only */
{
    if (bits & 2) {
        if (filled > q_next) { AU_QUEUE(buf[q_next % NB], BS * 2); q_next++; }
        else { AU_QUEUE(silence, BS * 2); underruns++; }
    }
    if (bits & 4) { AU_STOP(); stopped = 1; }    /* nothing queued in time: the engine stopped (restarted by the mixer) */
}

/* Sound lumps are PU_CACHE (purgeable), not PU_STATIC. Five playing sounds held 68 KB of static zone blocks at
   random places (DSBAREXP 18.6 KB, DSPLPAIN 15 KB, ...), which broke the free space into pieces too small for a 35 KB texture. A purge
   only happens inside Z_Malloc, in the main context like the mixer, so before using a channel the mixer checks that the lump is still
   cached at the same address (lumpinfo[].cache) and drops the channel if not: no read of freed memory. */
struct chan { const byte *data, *base; u32 len, pos, step; int vol, lump; };   /* pos, step: 16.16 fixed point in source samples */
static struct chan ch[NCH];

static void release(struct chan *c) { c->data = NULL; }
static int still_cached(const struct chan *c) { return c->lump >= 0 && (unsigned)c->lump < numlumps && lumpinfo[c->lump].cache == c->base; }

static void mix(short *out)
{
    static int acc[BS];
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < NCH; i++) {
        struct chan *c = &ch[i];
        if (!c->data) continue;
        if (!still_cached(c)) { release(c); continue; }
        for (int k = 0; k < BS; k++) {
            u32 s = c->pos >> 16;
            if (s >= c->len) { release(c); break; }
            acc[k] += ((int)c->data[s] - 128) * c->vol;
            c->pos += c->step;
        }
    }
    { extern void hx_music_render(int *, int); hx_music_render(acc, BS); }
    for (int k = 0; k < BS; k++) { int v = acc[k] * 2; out[k] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}

static void fill_ahead(void)
{
    while (filled < q_next + NB - 2) {           /* buffers q_next-2 (playing) and q_next-1 (queued) are busy */
        short *b = buf[filled % NB];
        mix(b);
        FW_WB(b, BS * 2);
        filled++;
    }
}

static void stream_start(void)
{
    q_next = 0; filled = 0; stopped = 0;
    fill_ahead();                                /* fills NB-2 = 2 buffers */
    AU_CB(au_event);
    AU_VOL(HW_VOLUME);
    AU_START(RATE, buf[0], BS * 2);
    AU_QUEUE(buf[1], BS * 2);                    /* tested: queued at once, the engine chains to it */
    q_next = 2;
    running = 1;
}

static boolean hx_Init(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    memset(silence, 0, sizeof silence);
    FW_WB(silence, sizeof silence);
    for (int i = 0; i < NCH; i++) ch[i].data = NULL;
    stream_start();
    return true;
}

int hx_sound_underruns(void) { int u = underruns; underruns = 0; return u; }   /* (profiling: silent buffers since the last call) */

void hx_sound_stop(void)                         /* (platform: before the camera powers off) */
{
    if (!running) return;
    running = 0;
    AU_STOP();
    AU_CB(0);
}

static void hx_Shutdown(void) { hx_sound_stop(); }

static int hx_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];
    if (sfx->link) sfx = sfx->link;
    name[0] = 'd'; name[1] = 's';
    strncpy(name + 2, DEH_String(sfx->name), 6); name[8] = 0;
    return W_GetNumForName(name);
}

extern unsigned hx_prof[];
static void hx_Update(void)
{
    if (!running) return;
    u32 t = hx_svc->now_ms();
    if (stopped) stream_start();                 /* the engine ran dry (very long frame): start again (one click) */
    else fill_ahead();
    hx_prof[4] += hx_svc->now_ms() - t;
}

static void hx_UpdateSoundParams(int channel, int vol, int sep)
{
    (void)sep;
    if (channel >= 0 && channel < NCH) ch[channel].vol = vol;
}

static int hx_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    (void)sep;
    if (channel < 0 || channel >= NCH) return -1;
    struct chan *c = &ch[channel];
    release(c);
    int lump = sfx->lumpnum;
    if (lump < 0 || (unsigned)lump >= numlumps || W_LumpLength(lump) < 8 + 32) return -1;
    const byte *d = W_CacheLumpNum(lump, PU_CACHE);    /* DMX format: u16 3, u16 rate, u32 samples (incl. 16 pad bytes each end) */
    u32 rate = d[2] | d[3] << 8, n = d[4] | d[5] << 8 | d[6] << 16 | (u32)d[7] << 24;
    u32 avail = (u32)W_LumpLength(lump) - 8;
    if (n > avail) n = avail;
    if (n <= 32 || !rate) return -1;
    c->lump = lump;
    c->base = d;
    c->len = n - 32;
    c->pos = 0;
    c->step = (u32)(((unsigned long long)rate << 16) / RATE);
    c->vol = vol;
    c->data = d + 8 + 16;                        /* set last: the mixer runs in the same (main) context, but keep it simple */
    return channel;
}

static void hx_StopSound(int channel) { if (channel >= 0 && channel < NCH) release(&ch[channel]); }
static boolean hx_SoundIsPlaying(int channel) { return channel >= 0 && channel < NCH && ch[channel].data != NULL; }
static void hx_CacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }   /* no precache: zone memory */

static snddevice_t hx_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module = {
    hx_devices, sizeof hx_devices / sizeof hx_devices[0],
    hx_Init, hx_Shutdown, hx_GetSfxLumpNum, hx_Update, hx_UpdateSoundParams, hx_StartSound, hx_StopSound, hx_SoundIsPlaying, hx_CacheSounds,
};

/* music: doom/hx_music.c (DG_music_module, hx_music_render) */
