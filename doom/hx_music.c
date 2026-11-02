/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 Andrew Spode */
/* Doom music on the camera: a small software synthesiser for Doom's own MUS lumps, mixed into the sound-effect stream (hx_sound.c calls
   hx_music_render for every 512-sample buffer, 11025 Hz mono). MUS: 16 channels (15 = percussion), events {release, play (+volume),
   pitch bend, system, controller, end of measure, score end} with delays in 140 Hz ticks. Voices: NV oscillators (phase accumulator) with
   a waveform and envelope chosen by the instrument family (General MIDI program / 8), noise and short thumps for the drums. Chiptune-ish
   by design: cheap (~2% CPU) and no extra files. doom/MUSIC.md compares the authentic options (OPL2, ADPCM, MP3). */
#include <string.h>
#include "services.h"
#include "doomtype.h"
#include "i_sound.h"

#define RATE 11025
#define NV   16                                  /* voices */
#define SPT_Q8 ((RATE << 8) / 140)                /* samples per MUS tick, Q8 (78.75) */

int hx_music_ok = 0;                             /* (s_sound.c patch 16: no music lump is cached while this is 0) */

static const byte *lump, *score, *score_end, *mus_pos;
static int playing, paused, looping, mus_volume = 100;
static int wait_q8;                              /* samples (Q8) until the next event */
/* generated (440 Hz * 2^((n-69)/12) * 2^32 / 11025; 2^((b-128)/768) * 65536): no maths library in RAM */
static const u32 note_inc[128] = {   /* (capped at half the sample rate) */
    3185014u, 3374405u, 3575058u, 3787642u, 4012867u, 4251484u, 4504291u, 4772130u,
    5055895u, 5356535u, 5675051u, 6012507u, 6370029u, 6748811u, 7150116u, 7575284u,
    8025734u, 8502969u, 9008582u, 9544260u, 10111791u, 10713070u, 11350102u, 12025014u,
    12740059u, 13497622u, 14300233u, 15150569u, 16051469u, 17005939u, 18017164u, 19088521u,
    20223583u, 21426140u, 22700205u, 24050029u, 25480118u, 26995245u, 28600466u, 30301138u,
    32102938u, 34011878u, 36034329u, 38177042u, 40447167u, 42852281u, 45400410u, 48100059u,
    50960237u, 53990491u, 57200933u, 60602277u, 64205876u, 68023756u, 72068659u, 76354085u,
    80894335u, 85704562u, 90800821u, 96200119u, 101920475u, 107980982u, 114401866u, 121204555u,
    128411752u, 136047513u, 144137319u, 152708170u, 161788670u, 171409125u, 181601642u, 192400238u,
    203840951u, 215961965u, 228803732u, 242409110u, 256823505u, 272095026u, 288274638u, 305416340u,
    323577341u, 342818251u, 363203285u, 384800476u, 407681903u, 431923931u, 457607464u, 484818220u,
    513647011u, 544190052u, 576549277u, 610832681u, 647154682u, 685636502u, 726406570u, 769600953u,
    815363807u, 863847862u, 915214929u, 969636440u, 1027294023u, 1088380105u, 1153098554u, 1221665362u,
    1294309365u, 1371273005u, 1452813141u, 1539201906u, 1630727614u, 1727695724u, 1830429858u, 1939272881u,
    2054588047u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u,
    2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u
};
static const u32 bend_mul[256] = {
    58385u, 58438u, 58491u, 58544u, 58597u, 58650u, 58702u, 58755u,
    58809u, 58862u, 58915u, 58968u, 59021u, 59075u, 59128u, 59181u,
    59235u, 59288u, 59342u, 59395u, 59449u, 59503u, 59556u, 59610u,
    59664u, 59718u, 59772u, 59826u, 59880u, 59934u, 59988u, 60042u,
    60096u, 60151u, 60205u, 60259u, 60314u, 60368u, 60423u, 60477u,
    60532u, 60586u, 60641u, 60696u, 60751u, 60806u, 60860u, 60915u,
    60970u, 61025u, 61081u, 61136u, 61191u, 61246u, 61301u, 61357u,
    61412u, 61468u, 61523u, 61579u, 61634u, 61690u, 61746u, 61801u,
    61857u, 61913u, 61969u, 62025u, 62081u, 62137u, 62193u, 62249u,
    62305u, 62362u, 62418u, 62474u, 62531u, 62587u, 62644u, 62700u,
    62757u, 62814u, 62870u, 62927u, 62984u, 63041u, 63098u, 63155u,
    63212u, 63269u, 63326u, 63383u, 63440u, 63498u, 63555u, 63612u,
    63670u, 63727u, 63785u, 63842u, 63900u, 63958u, 64016u, 64073u,
    64131u, 64189u, 64247u, 64305u, 64363u, 64421u, 64479u, 64538u,
    64596u, 64654u, 64713u, 64771u, 64830u, 64888u, 64947u, 65005u,
    65064u, 65123u, 65182u, 65240u, 65299u, 65358u, 65417u, 65476u,
    65536u, 65595u, 65654u, 65713u, 65773u, 65832u, 65891u, 65951u,
    66010u, 66070u, 66130u, 66189u, 66249u, 66309u, 66369u, 66429u,
    66489u, 66549u, 66609u, 66669u, 66729u, 66789u, 66850u, 66910u,
    66971u, 67031u, 67092u, 67152u, 67213u, 67273u, 67334u, 67395u,
    67456u, 67517u, 67578u, 67639u, 67700u, 67761u, 67822u, 67883u,
    67945u, 68006u, 68067u, 68129u, 68190u, 68252u, 68314u, 68375u,
    68437u, 68499u, 68561u, 68623u, 68685u, 68747u, 68809u, 68871u,
    68933u, 68995u, 69057u, 69120u, 69182u, 69245u, 69307u, 69370u,
    69432u, 69495u, 69558u, 69621u, 69684u, 69747u, 69809u, 69873u,
    69936u, 69999u, 70062u, 70125u, 70189u, 70252u, 70315u, 70379u,
    70442u, 70506u, 70570u, 70633u, 70697u, 70761u, 70825u, 70889u,
    70953u, 71017u, 71081u, 71145u, 71209u, 71274u, 71338u, 71403u,
    71467u, 71532u, 71596u, 71661u, 71725u, 71790u, 71855u, 71920u,
    71985u, 72050u, 72115u, 72180u, 72245u, 72310u, 72376u, 72441u,
    72507u, 72572u, 72638u, 72703u, 72769u, 72834u, 72900u, 72966u,
    73032u, 73098u, 73164u, 73230u, 73296u, 73362u, 73429u, 73495u
};
static u32 noise = 0x12345678;

struct mchan { u8 prog, vol, bend, last_vel; };
static struct mchan chn[16];
enum { W_TRI, W_SQUARE, W_SAW, W_NOISE };
struct voice { u8 on, chan, note, wave, rel, drum; u32 phase, inc; int env, decay, sustain, amp; };
static struct voice v[NV];

/* instrument families (GM program / 8): waveform, decay per 32 samples (Q12 env units), sustain level (Q12) */
static const struct { u8 wave; short decay, sustain; } fam[16] = {
    { W_TRI, 24, 1200 },    /* piano */            { W_TRI, 60, 0 },       /* chromatic percussion */
    { W_SQUARE, 4, 2600 },  /* organ */            { W_SAW, 20, 1600 },    /* guitar (Doom: lots of distortion guitar) */
    { W_TRI, 10, 2800 },    /* bass */             { W_SAW, 4, 2800 },     /* strings */
    { W_SAW, 4, 2600 },     /* ensemble */         { W_SAW, 8, 2600 },     /* brass */
    { W_SQUARE, 8, 2400 },  /* reed */             { W_TRI, 8, 2600 },     /* pipe */
    { W_SQUARE, 6, 2800 },  /* synth lead */       { W_TRI, 3, 3000 },     /* synth pad */
    { W_SAW, 10, 2000 },    /* synth effects */    { W_TRI, 20, 1000 },    /* ethnic */
    { W_TRI, 80, 0 },       /* percussive */       { W_NOISE, 40, 0 },     /* sound effects */
};


static void voice_inc(struct voice *x) { x->inc = (u32)(((unsigned long long)note_inc[x->note & 127] * bend_mul[chn[x->chan].bend]) >> 16); }

static void note_on(int c, int note, int vel)
{
    struct voice *x = NULL;
    for (int i = 0; i < NV; i++) if (!v[i].on) { x = &v[i]; break; }
    if (!x) {                                    /* steal the quietest releasing voice, else the quietest */
        for (int i = 0; i < NV; i++) if (!x || (v[i].rel && !x->rel) || (v[i].rel == x->rel && v[i].env < x->env)) x = &v[i];
    }
    memset(x, 0, sizeof *x);
    x->on = 1; x->chan = (u8)c; x->note = (u8)note; x->env = 4096;
    x->amp = vel * chn[c].vol / 127;             /* 0..127 */
    if (c == 15) {                               /* GM drums: noise for snares/hats/cymbals, low thumps for kicks and toms */
        x->drum = 1;
        if (note == 35 || note == 36) { x->wave = W_TRI; x->note = 28; x->decay = 160; }
        else if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) { x->wave = W_TRI; x->note = (u8)(note - 2); x->decay = 120; }
        else { x->wave = W_NOISE; x->decay = (note == 42 || note == 44) ? 400 : (note == 49 || note == 57 || note == 51) ? 40 : 140; }
        x->amp = x->amp * 3 / 4;
    } else {
        int f = chn[c].prog >> 3;
        x->wave = fam[f].wave; x->decay = fam[f].decay; x->sustain = fam[f].sustain;
    }
    voice_inc(x);
}
static void note_off(int c, int note)
{
    for (int i = 0; i < NV; i++) if (v[i].on && !v[i].drum && v[i].chan == c && v[i].note == note) v[i].rel = 1;
}
static void all_off(void) { for (int i = 0; i < NV; i++) v[i].on = 0; }

static void reset_channels(void)
{
    for (int c = 0; c < 16; c++) { chn[c].prog = 0; chn[c].vol = 100; chn[c].bend = 128; chn[c].last_vel = 100; }
}

static int next_byte(void) { return mus_pos < score_end ? *mus_pos++ : -1; }

static void events(void)                         /* process events until a delay (or the end) */
{
    for (;;) {
        int e = next_byte();
        if (e < 0) { playing = 0; all_off(); return; }
        int c = e & 15, type = (e >> 4) & 7, last = e & 0x80, a, b;
        switch (type) {
        case 0: a = next_byte(); note_off(c, a & 127); break;
        case 1: a = next_byte(); if (a & 0x80) chn[c].last_vel = (u8)(next_byte() & 127); note_on(c, a & 127, chn[c].last_vel); break;
        case 2: a = next_byte(); chn[c].bend = (u8)a;
                for (int i = 0; i < NV; i++) if (v[i].on && v[i].chan == c && !v[i].drum) voice_inc(&v[i]); break;
        case 3: a = next_byte(); if (a == 10 || a == 11) for (int i = 0; i < NV; i++) if (v[i].chan == c) v[i].rel = 1; break;
        case 4: a = next_byte(); b = next_byte();
                if (a == 0) chn[c].prog = (u8)(b & 127);
                else if (a == 3) chn[c].vol = (u8)(b & 127);
                break;
        case 5: break;                           /* end of measure */
        case 6:                                  /* score end */
            if (looping) { mus_pos = score; reset_channels(); all_off(); wait_q8 = 0; return; }
            playing = 0; all_off(); return;
        default: break;
        }
        if (last) {                              /* delay: 7-bit groups, high bit = more */
            int d = 0, x;
            do { x = next_byte(); if (x < 0) { playing = 0; return; } d = (d << 7) | (x & 127); } while (x & 0x80);
            wait_q8 += d * SPT_Q8;
            if (wait_q8 > 0) return;
        }
    }
}

static int sample(struct voice *x)               /* -1024..1023 */
{
    u32 p = x->phase;
    switch (x->wave) {
    case W_SQUARE: return (p >> 31) ? 700 : -700;
    case W_SAW:    return (int)(p >> 21) - 1024;
    case W_NOISE:  noise = noise * 1664525u + 1013904223u; return (int)(noise >> 21) - 1024;
    default: { int t = (int)(p >> 20); return t < 2048 ? t - 1024 : 3071 - t; }
    }
}

void hx_music_render(int *acc, int n)            /* add n samples of music into acc (called by the mixer, main context) */
{
    if (!playing || paused || !mus_volume) return;
    int k = 0;
    while (k < n) {
        while (wait_q8 < 256 && playing) events();
        if (!playing) return;
        int run = wait_q8 >> 8; if (run > n - k) run = n - k; if (run > 32) run = 32;
        for (int i = 0; i < NV; i++) {
            struct voice *x = &v[i];
            if (!x->on) continue;
            int g = x->env * x->amp / 127 * mus_volume / 127;    /* Q12 x 0..1 */
            for (int s = 0; s < run; s++) { acc[k + s] += (sample(x) * g) >> 11; x->phase += x->inc; }   /* (>> 13 was far quieter than the effects) */
            if (x->rel) x->env -= 300;                           /* release: ~0.4 s from full */
            else if (x->env > x->sustain) { x->env -= x->decay; if (x->env < x->sustain) x->env = x->sustain; }
            if (x->env <= 0 || (x->drum && x->env < 64)) x->on = 0;
        }
        k += run; wait_q8 -= run << 8;
    }
}

static boolean m_Init(void) { hx_music_ok = 1; return true; }
static void m_Shutdown(void) { playing = 0; all_off(); }
static void m_SetVolume(int vol) { mus_volume = vol < 0 ? 0 : vol > 127 ? 127 : vol; }
static void m_Pause(void) { paused = 1; }
static void m_Resume(void) { paused = 0; }
static void *m_Register(void *data, int len)
{
    const byte *d = data;
    if (len < 16 || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1A) return NULL;
    return data;                                 /* the lump itself (s_sound keeps it cached while it plays) */
}
static void m_Unregister(void *h) { if (h && (const byte *)h == lump) { playing = 0; all_off(); lump = NULL; } }
static void m_Play(void *h, boolean loop)
{
    const byte *d = h;
    if (!d) return;
    u32 len = d[4] | d[5] << 8, start = d[6] | d[7] << 8;
    playing = 0; all_off(); reset_channels();
    lump = d; score = d + start; score_end = score + len; mus_pos = score;
    looping = loop; paused = 0; wait_q8 = 0; playing = 1;
}
static void m_Stop(void) { playing = 0; all_off(); }
static boolean m_IsPlaying(void) { return playing; }
music_module_t DG_music_module = {
    NULL, 0, m_Init, m_Shutdown, m_SetVolume, m_Pause, m_Resume, m_Register, m_Unregister, m_Play, m_Stop, m_IsPlaying, NULL,
};
