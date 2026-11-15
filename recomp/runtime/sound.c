/* Sound-Treiber des ARM7 (Ersatz für den NitroSDK-SND-Treiber).
 *
 * Der ARM9 schickt über IPC-Kanal 7 Adressen von Befehlslisten (SND_FlushCommand). Der echte
 * ARM7 arbeitet sie ab, spielt Sequenzen (SSEQ) mit seinem Sequencer und betreibt die 16
 * Hardware-Kanäle. Über den gemeinsamen Speicher (SNDSharedWork) meldet er Spieler- und
 * Kanalstatus, Tick-Zähler und Sequenz-Variablen zurück; Alarme (für das Nachfüllen von
 * Streams) meldet er über die IPC-FIFO.
 *
 * Umfang: Befehle, Sequencer (Timing, Variablen, Sprünge, Schleifen), Noten mit Instrumenten
 * aus der Bank (SBNK: PCM/PSG/Rauschen, Drum-Sets, Key-Splits; Samples aus SWAR), Hüllkurven
 * (ADSR), Lautstärke/Pan/Tonhöhe in den Einheiten des SDK, Kanalvergabe nach Priorität,
 * direkte Kanäle (Streams), Alarme, Mixer (32728 Hz Stereo). Noch nicht: LFO/Modulation,
 * Portamento, Sweep, Capture.
 * Alles läuft im Takt der VBlanks des Spiels, also deterministisch. Speicherzugriffe gehen
 * an der Zeitmessung des ARM9 vorbei (der ARM7 läuft parallel). Siehe docs/sound.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "sound.h"

void arm7_reply(uint32_t tag, uint32_t flag, uint32_t data);   /* arm7_hle.c */
uint8_t *platform_mem(uint32_t addr);                           /* boot_test.c: ohne Zeitverbrauch */

#define TAG_SND 7

/* ---------------------------------------------------------------- Speicher (ARM7-Sicht) */

static uint8_t m8(uint32_t a) { uint8_t *p = platform_mem(a); return p ? *p : 0; }
static uint16_t m16(uint32_t a) { return (uint16_t)(m8(a) | m8(a + 1) << 8); }
static uint32_t m32(uint32_t a) { return m16(a) | (uint32_t)m16(a + 2) << 16; }
static void w8(uint32_t a, uint8_t v) { uint8_t *p = platform_mem(a); if (p) *p = v; }
static void w16(uint32_t a, uint16_t v) { w8(a, (uint8_t)v); w8(a + 1, (uint8_t)(v >> 8)); }
static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)v); w16(a + 2, (uint16_t)(v >> 16)); }

/* SNDSharedWork: +0 finishCommandTag, +4 playerStatus, +8 channelStatus (16 Bit),
 * +0x20 je Spieler 0x24 Bytes (16 lokale Variablen s16, Tick-Zähler u32), +0x260 globale Variablen */
static uint32_t shared;
#define SW_PLAYER(p) (shared + 0x20u + (uint32_t)(p) * 0x24u)
#define SW_GLOBAL (shared + 0x260u)

/* ---------------------------------------------------------------- Zeit */

#define CYCLES_PER_FRAME 560190u          /* 33,513982 MHz / 59,8261 Hz */
#define SND_FRAME_CYCLES (64u * 2728u)     /* Sequencer-Takt des Treibers, ~5,21 ms */
#define OS_TICK_CYCLES 64u                 /* OS_SetPeriodicAlarm-Ticks */
#define MIX_RATE 32728u
static uint64_t now_cycles, next_snd_frame = SND_FRAME_CYCLES;

/* ---------------------------------------------------------------- Dezibel (0,1 dB) */

#define DB_MIN (-723)
static int db_of(int x) {   /* Lautstärke 0..127 -> 0,1 dB (quadratisch wie das SDK) */
    if (x <= 0) return DB_MIN;
    if (x >= 127) return 0;
    int d = (int)lround(400.0 * log10(x / 127.0));
    return d < DB_MIN ? DB_MIN : d;
}
static double gain_of_db(int db) { return db <= DB_MIN ? 0.0 : pow(10.0, db / 200.0); }

/* ---------------------------------------------------------------- Kanäle */

enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
typedef struct {
    int active, format, loop;       /* format: 0 PCM8, 1 PCM16, 2 IMA-ADPCM, 3 PSG/Rauschen */
    uint32_t data, loop_start, length;   /* Bytes */
    uint32_t period;                /* Takte (16,76 MHz) pro Sample */
    int volume, shift, pan;         /* direkte Kanäle (Befehle 14-16) */
    double gain_l, gain_r;          /* Mischgewichte */
    uint64_t pos;                   /* Position in Samples, 32.32 */
    uint64_t step;                  /* pro Ausgabe-Sample (aus period, siehe set_period) */
    const uint8_t *host;            /* Daten im PC-Speicher (Hauptspeicher ist zusammenhängend) */
    uint32_t total_s, loop_s;       /* Länge / Schleifenstart in Samples */
    int adpcm_sample, adpcm_index, adpcm_loop_sample, adpcm_loop_index;
    uint32_t adpcm_at;
    int duty;                       /* PSG: 0-7, Rauschen: -1 */
    uint16_t lfsr;
    /* Sequencer-Note (owner >= 0) */
    int owner, track, key, velocity, prio, length_ticks, base_key, inst_pan;
    uint32_t base_period;
    int env, env_phase, attack, decay_rate, sustain_db, release_rate;
} Chan;
static Chan ch[16];
static uint16_t locked;

static void set_period(Chan *c, uint32_t period) {
    if (period == c->period && c->step) return;
    c->period = period;
    c->step = period ? ((uint64_t)16756991u << 32) / ((uint64_t)period * MIX_RATE) : 0;
}
/* Datenzeiger und Längen in Samples nach dem Einrichten eines Kanals */
static void chan_prepare(Chan *c) {
    c->host = c->data ? platform_mem(c->data) : NULL;
    uint32_t total = c->loop_start + c->length;
    if (c->format == 2) {
        c->total_s = total > 4 ? (total - 4) * 2 : 0;
        c->loop_s = c->loop_start > 4 ? (c->loop_start - 4) * 2 : 0;
    } else {
        uint32_t per = c->format == 0 ? 1 : 2;
        c->total_s = total / per; c->loop_s = c->loop_start / per;
    }
}            /* LOCK_CHANNEL: vom Sequencer nicht benutzen */

/* ---------------------------------------------------------------- Alarme */

typedef struct { int set, running; uint32_t tick, period; uint8_t id; uint64_t next; } Alarm;
static Alarm alarm_[8];

/* ---------------------------------------------------------------- Sequencer */

#define PLAYERS 16
#define TRACKS 32
typedef struct {
    int used, ended, note_wait, tie, mute, cmp, depth;
    uint32_t base, pc, stack[3];
    uint8_t loops[3];
    int32_t wait;
    int player;
    int8_t transpose, bend, ext_pan;
    uint8_t volume, volume2, pan, prio, bend_range;
    int16_t ext_volume, ext_pitch;
    int attack, decay, sustain, release;   /* 255 = vom Instrument */
    uint16_t program, chmask;
    int tie_chan;
} Track;
typedef struct {
    int active, prepared, paused;
    uint32_t seq_base, bank;
    int track[16];
    uint16_t tempo, tempo_ratio, tempo_counter, allow;
    uint8_t prio, volume;
    int16_t ext_fader;
    uint32_t ticks;
} Player;
static Player pl[PLAYERS];
static Track tr[TRACKS];
static uint32_t rand_state = 0x12345678u;

static uint16_t snd_rand(void) { rand_state = rand_state * 1664525u + 1013904223u; return (uint16_t)(rand_state >> 16); }

static int16_t get_var(int p, int v) {
    if (!shared || v < 0 || v >= 32) return 0;
    return (int16_t)(v < 16 ? m16(SW_PLAYER(p) + (uint32_t)v * 2) : m16(SW_GLOBAL + (uint32_t)(v - 16) * 2));
}
static void set_var(int p, int v, int16_t x) {
    if (!shared || v < 0 || v >= 32) return;
    if (v < 16) w16(SW_PLAYER(p) + (uint32_t)v * 2, (uint16_t)x); else w16(SW_GLOBAL + (uint32_t)(v - 16) * 2, (uint16_t)x);
}

/* ---------------------------------------------------------------- Hüllkurve (SDK-Tabellen) */

static int attack_rate(int a) {
    static const uint8_t tab[19] = { 0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143 };
    if (a < 0) a = 0;
    if (a > 127) a = 127;
    return a < 109 ? 255 - a : tab[127 - a];
}
static int fall_rate(int x) {   /* Decay/Release */
    if (x == 127) return 0xffff;
    if (x == 126) return 0x3c00;
    if (x < 50) return x * 2 + 1;
    return 0x1e00 / (126 - x);
}
#define ENV_MIN (-92544)   /* -723 dB*128 */

/* ---------------------------------------------------------------- Bank (SBNK) / Wellen (SWAR) */

typedef struct { int type; uint16_t wave, arc; uint8_t key, a, d, s, r, pan; } Inst;

/* Instrument zu Programm und Taste. 0 = keins */
static int bank_inst(uint32_t bank, int prg, int key, Inst *in) {
    if (!bank) return 0;
    uint32_t n = m32(bank + 0x38);
    if ((uint32_t)prg >= n) return 0;
    uint32_t rec = m32(bank + 0x3c + (uint32_t)prg * 4);
    int type = rec & 0xff;
    uint32_t off = bank + (rec >> 8);
    if (type == 16) {   /* Drum-Set: min, max, je Taste 12 Bytes */
        int lo = m8(off), hi = m8(off + 1);
        if (key < lo || key > hi) return 0;
        off += 2 + (uint32_t)(key - lo) * 12;
        type = m8(off); off += 2;
    } else if (type == 17) {   /* Key-Split: 8 Grenzen, dann Einträge */
        int i = 0;
        while (i < 8 && m8(off + (uint32_t)i) && key > m8(off + (uint32_t)i)) i++;
        if (i >= 8) return 0;
        off += 8 + (uint32_t)i * 12;
        type = m8(off); off += 2;
    }
    if (type < 1 || type > 4) return 0;
    in->type = type;
    in->wave = m16(off); in->arc = m16(off + 2);
    in->key = m8(off + 4); in->a = m8(off + 5); in->d = m8(off + 6); in->s = m8(off + 7); in->r = m8(off + 8); in->pan = m8(off + 9);
    return 1;
}

/* Welle (SWAV-Kopf + Daten) aus dem SWAR der Bank. 0 = nicht geladen */
static uint32_t bank_wave(uint32_t bank, const Inst *in) {
    if (in->type == 4) return 0;   /* direktes PCM (Adresse): hier nicht unterstützt */
    if (in->arc >= 4) return 0;
    uint32_t arc = m32(bank + 0x18 + (uint32_t)in->arc * 8);   /* SNDWaveArcLink.waveArc */
    if (!arc) return 0;
    if (in->wave >= m32(arc + 0x38)) return 0;
    uint32_t off = m32(arc + 0x3c + (uint32_t)in->wave * 4);
    return off ? arc + off : 0;
}

/* ---------------------------------------------------------------- Kanalvergabe */

static int chan_free_for(int type, int prio, uint16_t allowed) {
    static const int pcm_order[16] = { 4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13 };
    int best = -1, best_score = 1 << 30;
    for (int k = 0; k < 16; k++) {
        int i = type == 2 ? 8 + k : type == 3 ? 14 + k : pcm_order[k];
        if (type == 2 && i > 13) break;
        if (type == 3 && i > 15) break;
        if (!(allowed >> i & 1) || (locked >> i & 1)) continue;
        Chan *c = &ch[i];
        int score;
        if (!c->active) score = -1;
        else if (c->owner < 0) continue;                      /* direkter Kanal (Stream) */
        else score = c->prio * 4 + (c->env_phase == ENV_RELEASE ? 0 : 2);
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0 && ch[best].active && ch[best].prio > prio && ch[best].env_phase != ENV_RELEASE) return -1;
    return best;
}

static void note_on(Track *t, int key, int vel, int len) {
    Player *P = &pl[t->player];
    Inst in;
    if (!bank_inst(P->bank, t->program, key, &in)) return;
    int prio = P->prio + t->prio; if (prio > 255) prio = 255;
    int i = chan_free_for(in.type, prio, (uint16_t)(P->allow & t->chmask));
    if (i < 0) return;
    Chan *c = &ch[i];
    memset(c, 0, sizeof *c);
    if (in.type == 1) {
        uint32_t w = bank_wave(P->bank, &in);
        if (!w) return;
        c->format = m8(w); c->loop = m8(w + 1) ? 1 : 2;
        c->base_period = m16(w + 4);
        c->data = w + 12;
        c->loop_start = (uint32_t)m16(w + 6) * 4;
        c->length = m32(w + 8) * 4;
        if (c->format == 2) {
            uint32_t h = m32(c->data);
            c->adpcm_sample = (int16_t)h; c->adpcm_index = (h >> 16) & 0x7f; if (c->adpcm_index > 88) c->adpcm_index = 88;
        }
    } else {
        c->format = 3; c->duty = in.type == 2 ? in.wave & 7 : -1;
        c->base_period = 8006; c->lfsr = 0x7fff;
    }
    c->active = 1; c->owner = t->player; c->track = (int)(t - tr);
    c->key = key; c->velocity = vel; c->prio = prio; c->length_ticks = len;
    c->base_key = in.key; c->inst_pan = in.pan;
    c->attack = attack_rate(t->attack != 255 ? t->attack : in.a);
    c->decay_rate = fall_rate(t->decay != 255 ? t->decay : in.d);
    c->sustain_db = db_of(t->sustain != 255 ? t->sustain : in.s) * 128;
    c->release_rate = fall_rate(t->release != 255 ? t->release : in.r);
    c->env = ENV_MIN; c->env_phase = ENV_ATTACK;
    chan_prepare(c);
    set_period(c, c->base_period);
    if (t->tie) t->tie_chan = i;
}

static void release_track_notes(int track, int hard) {
    for (int i = 0; i < 16; i++) if (ch[i].active && ch[i].owner >= 0 && ch[i].track == track) {
        if (hard) ch[i].active = 0; else ch[i].env_phase = ENV_RELEASE;
    }
}

/* Lautstärke, Pan, Tonhöhe und Hüllkurve eines Sequencer-Kanals (einmal pro Treiber-Takt) */
static void chan_update(Chan *c) {
    Track *t = &tr[c->track];
    Player *P = &pl[c->owner];
    switch (c->env_phase) {
    case ENV_ATTACK:
        c->env = -((-c->env * c->attack) >> 8);
        if (c->env >= 0 || c->attack == 0) { c->env = 0; c->env_phase = ENV_DECAY; }
        break;
    case ENV_DECAY:
        c->env -= c->decay_rate;
        if (c->env <= c->sustain_db) { c->env = c->sustain_db; c->env_phase = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN: break;
    case ENV_RELEASE:
        c->env -= c->release_rate;
        if (c->env <= ENV_MIN) { c->active = 0; return; }
        break;
    }
    int db = db_of(c->velocity) + db_of(t->volume) + db_of(t->volume2) + db_of(P->volume)
             + (c->env >> 7) + t->ext_volume + P->ext_fader;
    if (t->mute) db = DB_MIN;
    double g = gain_of_db(db < DB_MIN ? DB_MIN : db);
    int pan = (int)t->pan + t->ext_pan + c->inst_pan - 64;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    c->gain_l = g * (127 - pan) / 127.0;
    c->gain_r = g * pan / 127.0;
    int pitch = (c->key - c->base_key) * 64 + (t->bend * t->bend_range * 64) / 128 + t->ext_pitch;
    double per = c->base_period * pow(2.0, -pitch / 768.0);
    set_period(c, per < 16 ? 16 : per > 65535 ? 65535 : (uint32_t)per);
}

/* ---------------------------------------------------------------- Spuren */

static uint32_t rd_var_len(Track *t) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) { uint8_t b = m8(t->pc++); v = v << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    return v;
}
enum { ARG_U8, ARG_S16, ARG_VLV };
static int32_t rd_arg(Track *t, int type, int prefix) {
    if (prefix == 0xa0) {
        int16_t lo = (int16_t)m16(t->pc), hi = (int16_t)m16(t->pc + 2);
        t->pc += 4;
        return lo + (int32_t)((snd_rand() * (uint32_t)(hi - lo + 1)) >> 16);
    }
    if (prefix == 0xa1) return get_var(t->player, m8(t->pc++));
    if (type == ARG_U8) return m8(t->pc++);
    if (type == ARG_S16) { int16_t v = (int16_t)m16(t->pc); t->pc += 2; return v; }
    return (int32_t)rd_var_len(t);
}

static int alloc_track(void) {
    for (int i = 0; i < TRACKS; i++) if (!tr[i].used) { memset(&tr[i], 0, sizeof tr[i]); tr[i].used = 1; return i; }
    return -1;
}
static void track_init(Track *t, int p, uint32_t base, uint32_t pc) {
    t->player = p; t->base = base; t->pc = pc; t->note_wait = 1; t->volume = 127; t->volume2 = 127;
    t->pan = 64; t->prio = 64; t->bend_range = 2; t->chmask = 0xffff; t->tie_chan = -1;
    t->attack = t->decay = t->sustain = t->release = 255;
}

static void player_stop(int p) {
    Player *P = &pl[p];
    for (int i = 0; i < 16; i++) if (P->track[i] >= 0) { release_track_notes(P->track[i], 0); tr[P->track[i]].used = 0; P->track[i] = -1; }
    P->active = P->prepared = P->paused = 0;
}

static void player_prepare(int p, uint32_t seq_base, uint32_t offset, uint32_t bank) {
    if (p < 0 || p >= PLAYERS) return;
    player_stop(p);
    Player *P = &pl[p];
    for (int i = 0; i < 16; i++) P->track[i] = -1;
    P->seq_base = seq_base; P->bank = bank;
    P->tempo = 120; P->tempo_ratio = 256; P->tempo_counter = 240;
    P->volume = 127; P->prio = 64; P->ticks = 0; P->ext_fader = 0;
    if (!P->allow) P->allow = 0xffff;
    uint32_t pc = seq_base + offset;
    if (m8(pc) == 0xfe) pc += 3;   /* Spuren reservieren */
    int t = alloc_track();
    if (t < 0) return;
    track_init(&tr[t], p, seq_base, pc);
    P->track[0] = t;
    P->active = 1; P->prepared = 1;
    if (shared) w32(SW_PLAYER(p) + 0x20, 0);
}

/* Eine Spur um einen Tick weiter; 1 = Spur zu Ende */
static int track_tick(Track *t) {
    Player *P = &pl[t->player];
    int ti = (int)(t - tr);
    for (int i = 0; i < 16; i++)   /* Notenlängen */
        if (ch[i].active && ch[i].owner >= 0 && ch[i].track == ti && ch[i].length_ticks > 0 && --ch[i].length_ticks == 0)
            ch[i].env_phase = ENV_RELEASE;
    if (t->ended) return 1;
    if (t->wait > 0 && --t->wait > 0) return 0;
    int guard = 0;
    while (t->wait == 0 && !t->ended) {
        if (++guard > 10000) { t->ended = 1; break; }
        int cond = 1, prefix = 0;
        uint8_t cmd = m8(t->pc++);
        if (cmd == 0xa2) { cond = t->cmp; cmd = m8(t->pc++); }
        if (cmd == 0xa0 || cmd == 0xa1) { prefix = cmd; cmd = m8(t->pc++); }
        if (cmd < 0x80) {                              /* Note */
            int vel = m8(t->pc++);
            int32_t len = rd_arg(t, ARG_VLV, prefix);
            if (!cond) continue;
            int key = cmd + t->transpose;
            if (key < 0) key = 0;
            if (key > 127) key = 127;
            if (t->tie && t->tie_chan >= 0 && ch[t->tie_chan].active && ch[t->tie_chan].track == ti) {
                ch[t->tie_chan].key = key; ch[t->tie_chan].velocity = vel;   /* gebundene Note: nur umstimmen */
            } else if (!t->mute) note_on(t, key, vel, t->tie ? 0 : len);
            if (t->note_wait) t->wait = len;
            continue;
        }
        switch (cmd & 0xf0) {
        case 0x80: {
            int32_t v = rd_arg(t, ARG_VLV, prefix);
            if (!cond) break;
            if (cmd == 0x80) t->wait = v;
            else if (cmd == 0x81) t->program = (uint16_t)v;
            break; }
        case 0x90: {
            if (cmd == 0x93) {
                uint8_t no = m8(t->pc); uint32_t off = m16(t->pc + 1) | (uint32_t)m8(t->pc + 3) << 16;
                t->pc += 4;
                if (!cond || no >= 16 || P->track[no] >= 0) break;
                int n = alloc_track();
                if (n < 0) break;
                track_init(&tr[n], t->player, t->base, t->base + off);
                P->track[no] = n;
            } else if (cmd == 0x94 || cmd == 0x95) {
                uint32_t off = m16(t->pc) | (uint32_t)m8(t->pc + 2) << 16;
                t->pc += 3;
                if (!cond) break;
                if (cmd == 0x95) { if (t->depth >= 3) break; t->stack[t->depth++] = t->pc; }
                t->pc = t->base + off;
            }
            break; }
        case 0xb0: {
            int v = m8(t->pc++);
            int32_t x = rd_arg(t, ARG_S16, prefix);
            if (!cond) break;
            int16_t cur = get_var(t->player, v);
            switch (cmd) {
            case 0xb0: set_var(t->player, v, (int16_t)x); break;
            case 0xb1: set_var(t->player, v, (int16_t)(cur + x)); break;
            case 0xb2: set_var(t->player, v, (int16_t)(cur - x)); break;
            case 0xb3: set_var(t->player, v, (int16_t)(cur * x)); break;
            case 0xb4: if (x) set_var(t->player, v, (int16_t)(cur / x)); break;
            case 0xb5: set_var(t->player, v, (int16_t)(x >= 0 ? cur << x : cur >> -x)); break;
            case 0xb6: {
                int neg = x < 0; if (neg) x = -x;
                int32_t r = (int32_t)((snd_rand() * (uint32_t)(x + 1)) >> 16);
                set_var(t->player, v, (int16_t)(neg ? -r : r)); break; }
            case 0xb8: t->cmp = cur == x; break;
            case 0xb9: t->cmp = cur >= x; break;
            case 0xba: t->cmp = cur > x; break;
            case 0xbb: t->cmp = cur <= x; break;
            case 0xbc: t->cmp = cur < x; break;
            case 0xbd: t->cmp = cur != x; break;
            default: break;
            }
            break; }
        case 0xc0: case 0xd0: {
            int32_t x = rd_arg(t, ARG_U8, prefix);
            if (!cond) break;
            switch (cmd) {
            case 0xc0: t->pan = (uint8_t)x; break;
            case 0xc1: t->volume = (uint8_t)x; break;
            case 0xc2: P->volume = (uint8_t)x; break;
            case 0xc3: t->transpose = (int8_t)x; break;
            case 0xc4: t->bend = (int8_t)x; break;
            case 0xc5: t->bend_range = (uint8_t)x; break;
            case 0xc6: t->prio = (uint8_t)x; break;
            case 0xc7: t->note_wait = x != 0; break;
            case 0xc8: t->tie = x != 0; if (!t->tie) { release_track_notes(ti, 0); t->tie_chan = -1; } break;
            case 0xd0: t->attack = x; break;
            case 0xd1: t->decay = x; break;
            case 0xd2: t->sustain = x; break;
            case 0xd3: t->release = x; break;
            case 0xd4: if (t->depth < 3) { t->stack[t->depth] = t->pc; t->loops[t->depth] = (uint8_t)x; t->depth++; } break;
            case 0xd5: t->volume2 = (uint8_t)x; break;
            default: break;   /* Modulation, Portamento: noch nicht */
            }
            break; }
        case 0xe0: {
            int32_t x = rd_arg(t, ARG_S16, prefix);
            if (!cond) break;
            if (cmd == 0xe1) P->tempo = (uint16_t)x;
            break; }
        case 0xf0:
            if (!cond) { if (cmd == 0xfe) t->pc += 2; break; }
            switch (cmd) {
            case 0xfc:
                if (t->depth == 0) break;
                if (t->loops[t->depth - 1] == 0) { t->pc = t->stack[t->depth - 1]; break; }
                if (--t->loops[t->depth - 1] == 0) { t->depth--; break; }
                t->pc = t->stack[t->depth - 1];
                break;
            case 0xfd: if (t->depth > 0) t->pc = t->stack[--t->depth]; break;
            case 0xfe: t->pc += 2; break;
            case 0xff: t->ended = 1; return 1;
            default: break;
            }
            break;
        default: break;
        }
    }
    return t->ended;
}

static void player_step(int p) {
    Player *P = &pl[p];
    int alive = 0;
    for (int i = 0; i < 16; i++) {
        int t = P->track[i];
        if (t < 0) continue;
        if (track_tick(&tr[t])) { release_track_notes(t, 0); tr[t].used = 0; P->track[i] = -1; } else alive++;
    }
    P->ticks++;
    if (!alive) player_stop(p);
}

/* Ein Treiber-Takt (~5,21 ms): Sequenzen und Hüllkurven */
static void sequencer_frame(void) {
    for (int p = 0; p < PLAYERS; p++) {
        Player *P = &pl[p];
        if (!P->active || P->prepared || P->paused) continue;
        while (P->tempo_counter >= 240 && P->active) { P->tempo_counter -= 240; player_step(p); }
        if (P->active) P->tempo_counter = (uint16_t)(P->tempo_counter + ((P->tempo * P->tempo_ratio) >> 8));
    }
    for (int i = 0; i < 16; i++) if (ch[i].active && ch[i].owner >= 0) chan_update(&ch[i]);
}

/* ---------------------------------------------------------------- Status */

static void update_shared(void) {
    if (!shared) return;
    uint32_t ps = 0;
    for (int p = 0; p < PLAYERS; p++) {
        if (pl[p].active) ps |= 1u << p;
        w32(SW_PLAYER(p) + 0x20, pl[p].ticks);
    }
    w32(shared + 4, ps);
    uint16_t cs = 0;
    for (int i = 0; i < 16; i++) if (ch[i].active) cs |= (uint16_t)(1u << i);
    w16(shared + 8, cs);
}

/* ---------------------------------------------------------------- Befehle */

static void direct_gain(Chan *c) {
    static const int div[4] = { 1, 2, 4, 16 };
    double g = c->volume / 127.0 / div[c->shift];
    c->gain_l = g * (127 - c->pan) / 127.0;
    c->gain_r = g * c->pan / 127.0;
}

static void chan_start(int i) {
    Chan *c = &ch[i];
    if (!c->data && c->format != 3) return;
    c->active = 1; c->pos = 0; c->adpcm_at = 0; c->owner = -1;
    if (c->format == 2) {
        uint32_t h = m32(c->data);
        c->adpcm_sample = (int16_t)h; c->adpcm_index = (h >> 16) & 0x7f; if (c->adpcm_index > 88) c->adpcm_index = 88;
    }
    c->lfsr = 0x7fff;
    chan_prepare(c);
    c->step = 0; set_period(c, c->period);
    direct_gain(c);
}

static void timer_cmd(int start, uint32_t chmask, uint32_t alarmmask) {
    for (int i = 0; i < 16; i++) if (chmask >> i & 1) { if (start) chan_start(i); else ch[i].active = 0; }
    for (int i = 0; i < 8; i++) if (alarmmask >> i & 1) {
        Alarm *a = &alarm_[i];
        if (start && a->set) { a->running = 1; a->next = now_cycles + (uint64_t)a->tick * OS_TICK_CYCLES; }
        else a->running = 0;
    }
}

static void track_param(uint32_t a0, uint32_t mask, uint32_t off, uint32_t val) {
    int p = a0 & 0xff;
    if (p >= PLAYERS) return;
    for (int i = 0; i < 16; i++) {
        if (!(mask >> i & 1) || pl[p].track[i] < 0) continue;
        Track *t = &tr[pl[p].track[i]];
        switch (off) {
        case 0x04: t->volume = (uint8_t)val; break;
        case 0x05: t->volume2 = (uint8_t)val; break;
        case 0x06: t->bend = (int8_t)val; break;
        case 0x07: t->bend_range = (uint8_t)val; break;
        case 0x08: t->pan = (uint8_t)val; break;
        case 0x09: t->ext_pan = (int8_t)val; break;
        case 0x0a: t->ext_volume = (int16_t)val; break;
        case 0x0c: t->ext_pitch = (int16_t)val; break;
        case 0x13: t->transpose = (int8_t)val; break;
        default: break;
        }
    }
}

static void run_command(uint32_t id, const uint32_t *a) {
    switch (id) {
    case 0: player_prepare((int)a[0], a[1], a[2], a[3]); if (a[0] < PLAYERS) pl[a[0]].prepared = 0; break;   /* START_SEQ */
    case 1: if (a[0] < PLAYERS) player_stop((int)a[0]); break;                                             /* STOP_SEQ */
    case 2: player_prepare((int)a[0], a[1], a[2], a[3]); break;                                            /* PREPARE_SEQ */
    case 3: if (a[0] < PLAYERS) pl[a[0]].prepared = 0; break;                                              /* START_PREPARED_SEQ */
    case 4: if (a[0] < PLAYERS) pl[a[0]].paused = a[1] != 0; break;                                        /* PAUSE_SEQ */
    case 5: if (a[0] < PLAYERS) for (uint32_t k = 0; k < a[1] && pl[a[0]].active; k++) player_step((int)a[0]); break;   /* SKIP_SEQ */
    case 6: {                                                                                              /* PLAYER_PARAM */
        if (a[0] >= PLAYERS) break;
        Player *P = &pl[a[0]];
        if (a[1] == 4) P->prio = (uint8_t)a[2];
        else if (a[1] == 5) P->volume = (uint8_t)a[2];
        else if (a[1] == 6) P->ext_fader = (int16_t)a[2];
        else if (a[1] == 0x18) P->tempo = (uint16_t)a[2];
        else if (a[1] == 0x1a) P->tempo_ratio = (uint16_t)a[2];
        break; }
    case 7: track_param(a[0], a[1], a[2], a[3]); break;                                                    /* TRACK_PARAM */
    case 8: {                                                                                              /* MUTE_TRACK */
        if (a[0] >= PLAYERS) break;
        for (int i = 0; i < 16; i++) if ((a[1] >> i & 1) && pl[a[0]].track[i] >= 0) {
            tr[pl[a[0]].track[i]].mute = a[2] != 0;
            if (a[2]) release_track_notes(pl[a[0]].track[i], a[2] == 2);
        }
        break; }
    case 9: if (a[0] < PLAYERS) pl[a[0]].allow = (uint16_t)a[2]; break;                                   /* ALLOCATABLE_CHANNEL */
    case 10: if (a[0] < PLAYERS) set_var((int)a[0], (int)a[1], (int16_t)a[2]); break;                       /* PLAYER_LOCAL_VAR */
    case 11: set_var(0, (int)a[0] + 16, (int16_t)a[1]); break;                                             /* PLAYER_GLOBAL_VAR */
    case 12: timer_cmd(1, a[0], a[2]); break;                                                              /* START_TIMER */
    case 13: timer_cmd(0, a[0], a[2]); break;                                                              /* STOP_TIMER */
    case 14: {                                                                                             /* SETUP_CHANNEL_PCM */
        int i = a[0] & 15;
        Chan *c = &ch[i];
        memset(c, 0, sizeof *c);
        c->owner = -1;
        c->period = a[0] >> 16;
        c->data = a[1];
        c->length = (a[2] & 0x3fffff) * 4;
        c->shift = (a[2] >> 22) & 3;
        c->volume = (a[2] >> 24) & 0x7f;
        c->loop_start = (a[3] & 0xffff) * 4;
        c->pan = (a[3] >> 16) & 0x7f;
        c->format = (a[3] >> 24) & 3;
        c->loop = (a[3] >> 26) & 3;
        break; }
    case 15: case 16: {                                                                                    /* SETUP_CHANNEL_PSG / NOISE */
        int i = a[0] & 15;
        Chan *c = &ch[i];
        memset(c, 0, sizeof *c);
        c->owner = -1; c->format = 3;
        c->duty = id == 15 ? (int)(a[0] >> 8 & 7) : -1;
        c->period = a[0] >> 16;
        c->volume = a[1] & 0x7f; c->shift = (a[1] >> 8) & 3; c->pan = (a[1] >> 16) & 0x7f;
        break; }
    case 18: {                                                                                             /* SETUP_ALARM */
        if (a[0] >= 8) break;
        Alarm *al = &alarm_[a[0]];
        al->set = 1; al->running = 0; al->tick = a[1]; al->period = a[2]; al->id = (uint8_t)a[3];
        break; }
    case 19: for (int i = 0; i < 16; i++) if ((a[0] >> i & 1) && ch[i].owner < 0) set_period(&ch[i], a[1]); break;   /* CHANNEL_TIMER */
    case 20: for (int i = 0; i < 16; i++) if ((a[0] >> i & 1) && ch[i].owner < 0) { ch[i].volume = a[1] & 0x7f; ch[i].shift = (a[1] >> 8) & 3; direct_gain(&ch[i]); } break;   /* CHANNEL_VOLUME */
    case 21: for (int i = 0; i < 16; i++) if ((a[0] >> i & 1) && ch[i].owner < 0) { ch[i].pan = a[1] & 0x7f; direct_gain(&ch[i]); } break;   /* CHANNEL_PAN */
    case 26:                                                                                               /* LOCK_CHANNEL */
        locked |= (uint16_t)a[0];
        for (int i = 0; i < 16; i++) if ((a[0] >> i & 1) && ch[i].owner >= 0) ch[i].active = 0;
        break;
    case 27: locked &= (uint16_t)~a[0]; break;                                                             /* UNLOCK_CHANNEL */
    case 28: for (int i = 0; i < 16; i++) if ((a[0] >> i & 1) && !(locked >> i & 1)) ch[i].active = 0; break;   /* STOP_UNLOCKED_CHANNEL */
    case 29: shared = a[0]; break;                                                                         /* SHARED_WORK */
    default: break;   /* Master-Lautstärke, Cache-Hinweise, Treiberinfo */
    }
}

void sound_command_list(uint32_t list) {
    int n = 0;
    for (uint32_t c = list; c && n < 256; c = m32(c), n++) {
        uint32_t a[4] = { m32(c + 8), m32(c + 12), m32(c + 16), m32(c + 20) };
        run_command(m32(c + 4), a);
    }
    HLE(SIMULIERT, "ARM7 Sound: Befehlsliste ausgefuehrt (Sequencer, Kanaele, Alarme)");
    update_shared();
}

/* ---------------------------------------------------------------- Mixer */

static const int16_t adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };
static const int adpcm_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

#define RING 65536
static int16_t ring[RING * 2];
static uint32_t ring_w, ring_r;

static int chan_sample(Chan *c, int *alive) {
    uint32_t s = (uint32_t)(c->pos >> 32);
    if (c->format == 3) {
        if (c->duty >= 0) return ((s & 7) <= (uint32_t)c->duty) ? 0x7fff : -0x7fff;
        return (c->lfsr & 1) ? -0x7fff : 0x7fff;
    }
    if (s >= c->total_s) {
        if (c->loop == 1 && c->total_s > c->loop_s) {
            s = c->loop_s + (s - c->loop_s) % (c->total_s - c->loop_s);
            c->pos = (uint64_t)s << 32 | (c->pos & 0xffffffffu);
            if (c->format == 2) { c->adpcm_sample = c->adpcm_loop_sample; c->adpcm_index = c->adpcm_loop_index; c->adpcm_at = c->loop_s; }
        } else { *alive = 0; return 0; }
    }
    const uint8_t *d = c->host;
    if (!d) { *alive = 0; return 0; }
    if (c->format == 0) return (int8_t)d[s] << 8;
    if (c->format == 1) return (int16_t)(d[s * 2] | d[s * 2 + 1] << 8);
    if (c->adpcm_at > s + 1) {   /* adpcm_at = Zahl dekodierter Samples; weiter zurück: neu dekodieren */
        c->adpcm_sample = (int16_t)(d[0] | d[1] << 8); c->adpcm_index = d[2] & 0x7f; if (c->adpcm_index > 88) c->adpcm_index = 88;
        c->adpcm_at = 0;
    }
    while (c->adpcm_at <= s) {
        if (c->adpcm_at == c->loop_s) { c->adpcm_loop_sample = c->adpcm_sample; c->adpcm_loop_index = c->adpcm_index; }
        uint8_t b = d[4 + c->adpcm_at / 2];
        int nib = (c->adpcm_at & 1) ? b >> 4 : b & 15;
        int step = adpcm_step[c->adpcm_index], diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        int v = (nib & 8) ? c->adpcm_sample - diff : c->adpcm_sample + diff;
        c->adpcm_sample = v < -0x7fff ? -0x7fff : v > 0x7fff ? 0x7fff : v;
        c->adpcm_index += adpcm_idx[nib & 7];
        if (c->adpcm_index < 0) c->adpcm_index = 0;
        if (c->adpcm_index > 88) c->adpcm_index = 88;
        c->adpcm_at++;
    }
    return c->adpcm_sample;
}

static void mix(uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        double l = 0, r = 0;
        for (int i = 0; i < 16; i++) {
            Chan *c = &ch[i];
            if (!c->active || !c->step) continue;
            int alive = 1;
            int v = chan_sample(c, &alive);
            if (!alive) { c->active = 0; continue; }
            l += v * c->gain_l;
            r += v * c->gain_r;
            uint32_t before = (uint32_t)(c->pos >> 32);
            c->pos += c->step;
            if (c->format == 3 && c->duty < 0)
                for (uint32_t s = before; s < (uint32_t)(c->pos >> 32); s++)
                    c->lfsr = (uint16_t)((c->lfsr & 1) ? (c->lfsr >> 1) ^ 0x6000 : c->lfsr >> 1);
        }
        l *= 0.5; r *= 0.5;   /* Spielraum für mehrere Kanäle */
        if (ring_w - ring_r < RING) {
            uint32_t w = ring_w++ % RING;
            ring[w * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            ring[w * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
        }
    }
}

int sound_take_samples(int16_t *out, int max) {
    int n = 0;
    while (n < max && ring_r != ring_w) {
        uint32_t i = ring_r++ % RING;
        out[n * 2] = ring[i * 2]; out[n * 2 + 1] = ring[i * 2 + 1];
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- Takt */

static void debug_wav(void);
static void debug_stat(void);

void sound_vblank(void) {
    static uint64_t mixed;
    uint64_t end = now_cycles + CYCLES_PER_FRAME;
    for (;;) {
        uint64_t t = next_snd_frame;
        int which = -1;
        for (int i = 0; i < 8; i++) if (alarm_[i].running && alarm_[i].next < t) { t = alarm_[i].next; which = i; }
        if (t > end) break;
        uint64_t want = t * MIX_RATE / 33513982u;
        if (want > mixed) { mix((uint32_t)(want - mixed)); mixed = want; }
        now_cycles = t;
        if (which < 0) { sequencer_frame(); next_snd_frame += SND_FRAME_CYCLES; }
        else {
            Alarm *a = &alarm_[which];
            arm7_reply(TAG_SND, 0, (uint32_t)which | (uint32_t)a->id << 8);
            HLE(SIMULIERT, "ARM7 Sound: Alarm an den ARM9 (Stream nachfuellen)");
            if (a->period) a->next += (uint64_t)a->period * OS_TICK_CYCLES; else a->running = 0;
        }
    }
    now_cycles = end;
    uint64_t want = end * MIX_RATE / 33513982u;
    if (want > mixed) { mix((uint32_t)(want - mixed)); mixed = want; }
    update_shared();
    debug_stat();
    debug_wav();
}

/* ---------------------------------------------------------------- Fehlersuche */

/* BIS_SNDSTAT=n: alle n Bilder aktive Spieler und Kanäle */
static void debug_stat(void) {
    static int every = -1; static unsigned fr;
    if (every < 0) every = getenv("BIS_SNDSTAT") ? atoi(getenv("BIS_SNDSTAT")) : 0;
    if (every <= 0 || ++fr % (unsigned)every) return;
    fprintf(stderr, "  [sndstat] Bild %u:", fr);
    for (int p = 0; p < PLAYERS; p++) if (pl[p].active)
        fprintf(stderr, " P%d(seq %08x tick %u%s)", p, pl[p].seq_base, pl[p].ticks, pl[p].prepared ? " vorbereitet" : "");
    for (int i = 0; i < 16; i++) if (ch[i].active) fprintf(stderr, " K%d%s", i, ch[i].owner >= 0 ? "" : "d");
    fputc('\n', stderr);
}

/* BIS_WAV=datei: ohne Fenster den Ton in eine WAV-Datei schreiben (zum Anhören) */
static void debug_wav(void) {
    static FILE *f; static int checked; static uint32_t bytes;
    if (!checked) {
        checked = 1;
        const char *path = getenv("BIS_WAV");
        if (path && (f = fopen(path, "wb"))) { uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, f); }
    }
    if (!f) return;
    int16_t buf[4096 * 2];
    int n;
    while ((n = sound_take_samples(buf, 4096)) > 0) { fwrite(buf, 4, (size_t)n, f); bytes += (uint32_t)n * 4; }
    /* Kopf jedes Mal aktualisieren: die Datei bleibt auch bei Abbruch abspielbar */
    long pos = ftell(f);
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                      0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t v[3] = { bytes + 36, MIX_RATE, MIX_RATE * 4 };
    memcpy(h + 4, &v[0], 4); memcpy(h + 24, &v[1], 4); memcpy(h + 28, &v[2], 4); memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, pos, SEEK_SET);
    fflush(f);
}
