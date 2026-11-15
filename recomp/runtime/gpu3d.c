/* 3D-Engine des DS (Software) – siehe gpu3d.h.
 *
 * Konventionen der Hardware: Zeilenvektoren (v' = v * M), Festkomma 20.12 für
 * Matrizen und Koordinaten, Texturkoordinaten 12.4, Farben intern 6 Bit pro Kanal.
 *
 * Nachgebildet:  alle Matrix-Befehle, Vertex-Formate, Dreiecke/Vierecke/Streifen,
 *                Clipping an allen 6 Ebenen, Culling, Licht (4 Lichter, Shininess-Tabelle),
 *                Texturkoordinaten-Transformation (Quelle Texcoord, Normale, Vertex),
 *                alle 7 Texturformaten, Modulate/Decal/Toon/Highlight, Alpha-Test,
 *                Alpha-Blending, Z- und W-Buffer, Drahtgitter (Alpha 0), Clear-Farbe/-Tiefe.
 * Fehlt:         Schatten-Polygone, Nebel, Kantenmarkierung, Anti-Aliasing,
 *                Rear-Plane-Bitmap, genauer Box-Test (meldet immer "sichtbar").
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu3d.h"
#include "vram.h"

#define W 256
#define H 192
#define MAX_POLYS 2048
#define MAX_VERTS 6144

static uint8_t *io;
static inline uint16_t r16(uint32_t o) { return (uint16_t)(io[o] | io[o + 1] << 8); }
static inline uint32_t r32(uint32_t o) { return r16(o) | (uint32_t)r16(o + 2) << 16; }
static inline void w16(uint32_t o, uint16_t v) { io[o] = (uint8_t)v; io[o + 1] = (uint8_t)(v >> 8); }
static inline void w32(uint32_t o, uint32_t v) { w16(o, (uint16_t)v); w16(o + 2, (uint16_t)(v >> 16)); }

static unsigned flags;
static uint64_t cmd_count;
static unsigned dbg_in, dbg_far, dbg_clip, dbg_cull;   /* Fehlersuche: wo Polygone verloren gehen */

/* ================================================================ Matrizen */

typedef int32_t Mtx[16];   /* m[zeile * 4 + spalte] */

static Mtx proj, pos, vec, tex, clip;
static Mtx proj_st[1], pos_st[32], vec_st[32], tex_st[1];
static int proj_sp, pos_sp, tex_sp, mtx_mode, stack_err, clip_dirty = 1;

static void mtx_ident(Mtx m) { memset(m, 0, sizeof(Mtx)); m[0] = m[5] = m[10] = m[15] = 4096; }

static void mtx_mul(Mtx out, const Mtx a, const Mtx b) {   /* out = a * b */
    Mtx r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++) s += (int64_t)a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = (int32_t)(s >> 12);
        }
    memcpy(out, r, sizeof r);
}

static void update_clip(void) {
    if (clip_dirty) { mtx_mul(clip, pos, proj); clip_dirty = 0; }
}

/* Lädt (load=1) oder multipliziert m auf die aktuelle(n) Matrix(en).
 * Modus 2 betrifft Positions- und Richtungsmatrix, Skalierung aber nur die Position. */
static void mtx_apply(const Mtx m, int load, int scale) {
    Mtx *t[2] = { NULL, NULL };
    switch (mtx_mode) {
    case 0: t[0] = &proj; break;
    case 1: t[0] = &pos; break;
    case 2: t[0] = &pos; if (!scale) t[1] = &vec; break;
    case 3: t[0] = &tex; break;
    }
    for (int i = 0; i < 2; i++) {
        if (!t[i]) continue;
        if (load) memcpy(*t[i], m, sizeof(Mtx));
        else mtx_mul(*t[i], m, *t[i]);
    }
    clip_dirty = 1;
}

/* ================================================================ Geometrie-Zustand */

typedef struct {
    int32_t x, y, z, w;          /* Clip-Koordinaten, 20.12 */
    int r, g, b;                 /* 6 Bit */
    int32_t s, t;                /* 12.4 */
} Vtx;

typedef struct {
    float x, y, z, w;            /* Bildschirm x/y, Tiefe, w */
    float r, g, b, s, t;
} SVtx;

typedef struct {
    int n;
    SVtx v[10];
    uint32_t attr, texp, pltt;
    int translucent;
    float ymin, ymax;
} Poly;

/* zwei Listen: in Arbeit / zum Zeichnen übernommen */
static Poly polys[2][MAX_POLYS];
static unsigned npolys[2], nverts[2];
static int cur_list;
static int swap_pending, pending_valid, pending_list, pending_sort, pending_wbuf;
static int list_sort[2], list_wbuf[2];
static uint32_t pending_viewport_dummy;

static int vtx_r = 62, vtx_g = 62, vtx_b = 62;   /* aktuelle Vertexfarbe (6 Bit) */
static int32_t tc_s, tc_t, raw_s, raw_t;
static int16_t last_x, last_y, last_z;
static int32_t normal[3];                       /* .9 */
static uint32_t poly_attr, poly_attr_cur, texparam, plttbase;
static int prim, prim_count;
static Vtx prim_v[4];
static unsigned gx_vblanks;                        /* VBlank-Zähler (Fehlersuche) */
static uint32_t viewport = 0xbfff0000u;          /* x1=0 y1=0 x2=255 y2=191 */

/* Licht */
static int32_t light_vec[4][3];                  /* transformiert, .9 */
static uint16_t light_col[4];
static uint16_t dif, amb, spe, emi;
static int use_shininess;
static uint8_t shininess[128];

/* Testergebnisse */
static int32_t pos_result[4];
static int16_t vec_result[3];
static int box_result = 1;

static void c5to6(uint16_t c, int *r, int *g, int *b) {
    int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
    *r = r5 ? r5 * 2 + 1 : 0; *g = g5 ? g5 * 2 + 1 : 0; *b = b5 ? b5 * 2 + 1 : 0;
}

static int32_t sx10(uint32_t v) { return (int32_t)(v << 22) >> 22; }   /* 10 Bit mit Vorzeichen */

/* ================================================================ Polygone zusammensetzen */

static int clip_plane(SVtx *in, int n, SVtx *out, int axis, int sign) {
    /* Ebene: sign * coord <= w   (coord = x/y/z); in homogenen Koordinaten (vor Division) */
    int m = 0;
    for (int i = 0; i < n; i++) {
        SVtx *a = &in[i], *b = &in[(i + 1) % n];
        float ca = (axis == 0 ? a->x : axis == 1 ? a->y : a->z) * (float)sign;
        float cb = (axis == 0 ? b->x : axis == 1 ? b->y : b->z) * (float)sign;
        float da = a->w - ca, db = b->w - cb;   /* >= 0: innen */
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            SVtx v;
            v.x = a->x + (b->x - a->x) * t; v.y = a->y + (b->y - a->y) * t;
            v.z = a->z + (b->z - a->z) * t; v.w = a->w + (b->w - a->w) * t;
            v.r = a->r + (b->r - a->r) * t; v.g = a->g + (b->g - a->g) * t; v.b = a->b + (b->b - a->b) * t;
            v.s = a->s + (b->s - a->s) * t; v.t = a->t + (b->t - a->t) * t;
            out[m++] = v;
        }
        if (m >= 10) break;
    }
    return m;
}

static void emit_poly(const Vtx *vs, int n) {
    unsigned L = (unsigned)cur_list;
    if (npolys[L] >= MAX_POLYS || nverts[L] + (unsigned)n > MAX_VERTS) { flags |= G3D_MISS_OVERFLOW; return; }
    SVtx a[10], b[10];
    int far_vis = (poly_attr_cur >> 12) & 1;
    dbg_in++;
    if (dbg_in == 1 && getenv("BIS_3D")) {
        const char *nm[3] = { "proj", "pos", "clip" };
        const int32_t *mm[3] = { proj, pos, clip };
        for (int k = 0; k < 3; k++) {
            fprintf(stderr, "  [3d] %s:", nm[k]);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %d", mm[k][i]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  [3d] letzter Vertex roh %d %d %d\n", last_x, last_y, last_z);
    }
    static int frame_polys; static unsigned frame_seen = ~0u;
    if (frame_seen != gx_vblanks) { frame_seen = gx_vblanks; frame_polys = 0; }
    static long dbg_frame = -2;   /* BIS_3DTEX einmal lesen: getenv ist pro Polygon zu teuer */
    if (dbg_frame == -2) dbg_frame = getenv("BIS_3DTEX") ? atol(getenv("BIS_3DTEX")) : -1;
    int frame_dbg = dbg_frame >= 0 && (long)gx_vblanks == dbg_frame && frame_polys++ < 10;
    if (frame_dbg) {
        fprintf(stderr, "  [3df] poly attr %08x tex %08x roh-letzt %d %d %d pos12-15 %d %d %d %d proj0/5/12/13/15 %d %d %d %d %d\n",
                poly_attr_cur, texparam, last_x, last_y, last_z, pos[12], pos[13], pos[14], pos[15], proj[0], proj[5], proj[12], proj[13], proj[15]);
        for (int i = 0; i < n; i++) fprintf(stderr, "  [3df]   v%d clip %d %d %d %d\n", i, vs[i].x, vs[i].y, vs[i].z, vs[i].w);
    }
    if (dbg_in <= 4 && getenv("BIS_3D"))
        for (int i = 0; i < n; i++)
            fprintf(stderr, "  [3d] poly %u v%d clip %d %d %d %d attr %08x tex %08x\n", dbg_in, i,
                    vs[i].x, vs[i].y, vs[i].z, vs[i].w, poly_attr_cur, texparam);
    for (int i = 0; i < n; i++) {
        a[i].x = (float)vs[i].x; a[i].y = (float)vs[i].y; a[i].z = (float)vs[i].z; a[i].w = (float)vs[i].w;
        a[i].r = (float)vs[i].r; a[i].g = (float)vs[i].g; a[i].b = (float)vs[i].b;
        a[i].s = (float)vs[i].s; a[i].t = (float)vs[i].t;
        if (!far_vis && vs[i].z > vs[i].w) { dbg_far++; return; }   /* schneidet die ferne Ebene: verwerfen */
    }
    int m = n;
    SVtx *src = a, *dst = b;
    for (int axis = 2; axis >= 0 && m >= 3; axis--) {        /* z zuerst (nahe Ebene) */
        for (int sign = -1; sign <= 1 && m >= 3; sign += 2) {
            m = clip_plane(src, m, dst, axis, sign);
            SVtx *tmp = src; src = dst; dst = tmp;
        }
    }
    if (m < 3) { dbg_clip++; return; }

    /* Projektion + Viewport */
    int x1 = viewport & 0xff, y1 = (viewport >> 8) & 0xff, x2 = (viewport >> 16) & 0xff, y2 = viewport >> 24;
    float vw = (float)(x2 - x1 + 1), vh = (float)(y2 - y1 + 1);
    Poly *p = &polys[L][npolys[L]];
    p->ymin = 1e9f; p->ymax = -1e9f;
    for (int i = 0; i < m; i++) {
        SVtx *v = &src[i];
        float w = v->w > 0 ? v->w : 1;
        SVtx o = *v;
        o.x = ((v->x / w) + 1.0f) * vw * 0.5f + (float)x1;
        o.y = (float)H - (((v->y / w) + 1.0f) * vh * 0.5f + (float)y1);
        o.z = v->z / w;              /* -1..1 */
        o.w = w;
        p->v[i] = o;
        if (o.y < p->ymin) p->ymin = o.y;
        if (o.y > p->ymax) p->ymax = o.y;
    }
    p->n = m;
    /* Culling: auf dem Bildschirm im Uhrzeigersinn = Vorderseite */
    float area = 0;
    for (int i = 0; i < m; i++) {
        SVtx *u = &p->v[i], *v = &p->v[(i + 1) % m];
        area += u->x * v->y - v->x * u->y;
    }
    int front = area > 0;
    if (area != 0) {
        if (front && !((poly_attr_cur >> 7) & 1)) { dbg_cull++; return; }
        if (!front && !((poly_attr_cur >> 6) & 1)) { dbg_cull++; return; }
    }
    p->attr = poly_attr_cur;
    p->texp = texparam;
    p->pltt = plttbase;
    int alpha = (poly_attr_cur >> 16) & 31, fmt = (texparam >> 26) & 7;
    p->translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
    npolys[L]++;
    nverts[L] += (unsigned)n;
}

static void add_vertex(int32_t x, int32_t y, int32_t z) {
    last_x = (int16_t)x; last_y = (int16_t)y; last_z = (int16_t)z;
    update_clip();
    Vtx v;
    v.x = (int32_t)(((int64_t)x * clip[0] + (int64_t)y * clip[4] + (int64_t)z * clip[8] + 4096ll * clip[12]) >> 12);
    v.y = (int32_t)(((int64_t)x * clip[1] + (int64_t)y * clip[5] + (int64_t)z * clip[9] + 4096ll * clip[13]) >> 12);
    v.z = (int32_t)(((int64_t)x * clip[2] + (int64_t)y * clip[6] + (int64_t)z * clip[10] + 4096ll * clip[14]) >> 12);
    v.w = (int32_t)(((int64_t)x * clip[3] + (int64_t)y * clip[7] + (int64_t)z * clip[11] + 4096ll * clip[15]) >> 12);
    v.r = vtx_r; v.g = vtx_g; v.b = vtx_b;
    if (((texparam >> 30) & 3) == 3) {   /* Texturkoordinaten aus dem Vertex */
        v.s = (int32_t)(((int64_t)x * tex[0] + (int64_t)y * tex[4] + (int64_t)z * tex[8]) >> 24) + raw_s;
        v.t = (int32_t)(((int64_t)x * tex[1] + (int64_t)y * tex[5] + (int64_t)z * tex[9]) >> 24) + raw_t;
        flags |= G3D_MISS_TEXGEN;
    } else { v.s = tc_s; v.t = tc_t; }

    prim_v[prim_count < 4 ? prim_count : 3] = v;
    prim_count++;
    switch (prim) {
    case 0:   /* Dreiecke */
        if (prim_count == 3) { emit_poly(prim_v, 3); prim_count = 0; }
        break;
    case 1:   /* Vierecke */
        if (prim_count == 4) { emit_poly(prim_v, 4); prim_count = 0; }
        break;
    case 2: { /* Dreiecksstreifen */
        static int odd;
        if (prim_count == 3) {
            Vtx t[3];
            if (odd) { t[0] = prim_v[1]; t[1] = prim_v[0]; t[2] = prim_v[2]; }
            else { t[0] = prim_v[0]; t[1] = prim_v[1]; t[2] = prim_v[2]; }
            emit_poly(t, 3);
            odd ^= 1;
            prim_v[0] = prim_v[1]; prim_v[1] = prim_v[2]; prim_count = 2;
        }
        if (prim_count == 1) odd = 0;
        break; }
    case 3:   /* Vierecksstreifen: v0 v1 v3 v2 */
        if (prim_count == 4) {
            Vtx q[4] = { prim_v[0], prim_v[1], prim_v[3], prim_v[2] };
            emit_poly(q, 4);
            prim_v[0] = prim_v[2]; prim_v[1] = prim_v[3]; prim_count = 2;
        }
        break;
    }
}

/* ================================================================ Licht */

static void do_lighting(void) {
    /* Normale mit der Richtungsmatrix drehen (.9 -> .9) */
    float n[3];
    for (int i = 0; i < 3; i++)
        n[i] = (float)(((int64_t)normal[0] * vec[0 * 4 + i] + (int64_t)normal[1] * vec[1 * 4 + i]
                        + (int64_t)normal[2] * vec[2 * 4 + i]) >> 12) / 512.0f;
    float r = (float)(emi & 31), g = (float)((emi >> 5) & 31), b = (float)((emi >> 10) & 31);
    for (int l = 0; l < 4; l++) {
        if (!((poly_attr >> l) & 1)) continue;
        float L[3] = { light_vec[l][0] / 512.0f, light_vec[l][1] / 512.0f, light_vec[l][2] / 512.0f };
        float d = -(L[0] * n[0] + L[1] * n[1] + L[2] * n[2]);
        if (d < 0) d = 0;
        /* Halbvektor zwischen Licht und Blickrichtung (0,0,-1) */
        float h[3] = { (L[0]) / 2, (L[1]) / 2, (L[2] - 1.0f) / 2 };
        float s = -(h[0] * n[0] + h[1] * n[1] + h[2] * n[2]);
        if (s < 0) s = 0;
        s = s * s;
        if (s > 1) s = 1;
        if (use_shininess) s = shininess[(int)(s * 127.0f) & 127] / 255.0f;
        int lc = light_col[l];
        float lr = (float)(lc & 31), lg = (float)((lc >> 5) & 31), lb = (float)((lc >> 10) & 31);
        r += lr * ((float)(dif & 31) * d + (float)(spe & 31) * s + (float)(amb & 31)) / 31.0f;
        g += lg * ((float)((dif >> 5) & 31) * d + (float)((spe >> 5) & 31) * s + (float)((amb >> 5) & 31)) / 31.0f;
        b += lb * ((float)((dif >> 10) & 31) * d + (float)((spe >> 10) & 31) * s + (float)((amb >> 10) & 31)) / 31.0f;
    }
    int ri = (int)r, gi = (int)g, bi = (int)b;
    if (ri > 31) ri = 31;
    if (gi > 31) gi = 31;
    if (bi > 31) bi = 31;
    c5to6((uint16_t)(ri | gi << 5 | bi << 10), &vtx_r, &vtx_g, &vtx_b);
}

/* ================================================================ Befehle */

static const int8_t nparams[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1a] = 9, [0x1b] = 3, [0x1c] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};

/* Fehlersuche: BIS_GXLOG=n gibt alle GX-Befehle des n-ten VBlanks aus (Befehl + Parameter) */
static long gx_log_frame = -2;
static void gx_log(uint8_t cmd, const uint32_t *p) {
    static const uint8_t np[256] = { [0x10]=1,[0x11]=0,[0x12]=1,[0x13]=1,[0x14]=1,[0x15]=0,[0x16]=16,[0x17]=12,[0x18]=16,
        [0x19]=12,[0x1a]=9,[0x1b]=3,[0x1c]=3,[0x20]=1,[0x21]=1,[0x22]=1,[0x23]=2,[0x24]=1,[0x25]=1,[0x26]=1,[0x27]=1,
        [0x28]=1,[0x29]=1,[0x2a]=1,[0x2b]=1,[0x30]=1,[0x31]=1,[0x32]=1,[0x33]=1,[0x34]=32,[0x40]=1,[0x41]=0,[0x50]=1,
        [0x60]=1,[0x70]=3,[0x71]=2,[0x72]=1 };
    if (gx_log_frame == -2) gx_log_frame = getenv("BIS_GXLOG") ? atol(getenv("BIS_GXLOG")) : -1;
    if (gx_log_frame >= 0 && cmd == 0x60) fprintf(stderr, "  [gx] VIEWPORT %08x (VBlank %u)\n", p[0], gx_vblanks);   /* immer */
    {   /* BIS_GXMTX=a-b: nur Matrix-Befehle (0x10-0x1c) in den VBlanks a..b */
        static long ma = -2, mb = -1;
        if (ma == -2) { const char *e = getenv("BIS_GXMTX"); ma = -1; if (e) { ma = atol(e); const char *d = strchr(e, '-'); mb = d ? atol(d + 1) : ma; } }
        if (ma >= 0 && cmd >= 0x10 && cmd <= 0x1c && (long)gx_vblanks >= ma && (long)gx_vblanks <= mb && (cmd != 0x10 || p[0] == 0)) {
            fprintf(stderr, "  [gxm] V%u mode%u %02x", gx_vblanks, mtx_mode, cmd);
            for (int i = 0; i < np[cmd]; i++) fprintf(stderr, " %08x", p[i]);
            fprintf(stderr, "\n");
        }
    }
    if (gx_log_frame < 0 || (long)gx_vblanks != gx_log_frame) return;
    fprintf(stderr, "  [gx] %02x", cmd);
    for (int i = 0; i < np[cmd]; i++) fprintf(stderr, " %08x", p[i]);
    fprintf(stderr, "\n");
}

static void exec_cmd(uint8_t cmd, const uint32_t *p) {
    cmd_count++;
    gx_log(cmd, p);
    Mtx m;
    switch (cmd) {
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11:   /* PUSH */
        if (mtx_mode == 0) { memcpy(proj_st[0], proj, sizeof(Mtx)); if (proj_sp) stack_err = 1; proj_sp = 1; }
        else if (mtx_mode == 3) { memcpy(tex_st[0], tex, sizeof(Mtx)); tex_sp = 1; }
        else {
            if (pos_sp >= 31) stack_err = 1;
            memcpy(pos_st[pos_sp & 31], pos, sizeof(Mtx)); memcpy(vec_st[pos_sp & 31], vec, sizeof(Mtx));
            pos_sp = (pos_sp + 1) & 63;
        }
        break;
    case 0x12: { /* POP */
        int n = (int32_t)(p[0] << 26) >> 26;
        if (mtx_mode == 0) { proj_sp = 0; memcpy(proj, proj_st[0], sizeof(Mtx)); }
        else if (mtx_mode == 3) { tex_sp = 0; memcpy(tex, tex_st[0], sizeof(Mtx)); }
        else {
            pos_sp = (pos_sp - n) & 63;
            if (pos_sp >= 31) stack_err = 1;
            memcpy(pos, pos_st[pos_sp & 31], sizeof(Mtx)); memcpy(vec, vec_st[pos_sp & 31], sizeof(Mtx));
        }
        clip_dirty = 1;
        break; }
    case 0x13: { /* STORE */
        int i = p[0] & 31;
        if (mtx_mode == 0) memcpy(proj_st[0], proj, sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex_st[0], tex, sizeof(Mtx));
        else { if (i == 31) stack_err = 1; memcpy(pos_st[i], pos, sizeof(Mtx)); memcpy(vec_st[i], vec, sizeof(Mtx)); }
        break; }
    case 0x14: { /* RESTORE */
        int i = p[0] & 31;
        if (mtx_mode == 0) memcpy(proj, proj_st[0], sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex, tex_st[0], sizeof(Mtx));
        else { if (i == 31) stack_err = 1; memcpy(pos, pos_st[i], sizeof(Mtx)); memcpy(vec, vec_st[i], sizeof(Mtx)); }
        clip_dirty = 1;
        break; }
    case 0x15: mtx_ident(m); mtx_apply(m, 1, 0); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; mtx_apply(m, 1, 0); break;
    case 0x18: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; mtx_apply(m, 0, 0); break;
    case 0x17: case 0x19:
        for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 4096 : 0; }
        mtx_apply(m, cmd == 0x17, 0);
        break;
    case 0x1a:
        mtx_ident(m);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c];
        mtx_apply(m, 0, 0);
        break;
    case 0x1b: mtx_ident(m); m[0] = (int32_t)p[0]; m[5] = (int32_t)p[1]; m[10] = (int32_t)p[2]; mtx_apply(m, 0, 1); break;
    case 0x1c: mtx_ident(m); m[12] = (int32_t)p[0]; m[13] = (int32_t)p[1]; m[14] = (int32_t)p[2]; mtx_apply(m, 0, 0); break;

    case 0x20: c5to6((uint16_t)p[0], &vtx_r, &vtx_g, &vtx_b); break;
    case 0x21:   /* NORMAL */
        normal[0] = sx10(p[0]); normal[1] = sx10(p[0] >> 10); normal[2] = sx10(p[0] >> 20);
        if (((texparam >> 30) & 3) == 2) {
            tc_s = (int32_t)(((int64_t)normal[0] * tex[0] + (int64_t)normal[1] * tex[4] + (int64_t)normal[2] * tex[8]) >> 21) + raw_s;
            tc_t = (int32_t)(((int64_t)normal[0] * tex[1] + (int64_t)normal[1] * tex[5] + (int64_t)normal[2] * tex[9]) >> 21) + raw_t;
            flags |= G3D_MISS_TEXGEN;
        }
        if (poly_attr & 15) do_lighting();
        break;
    case 0x22:   /* TEXCOORD */
        raw_s = (int16_t)(p[0] & 0xffff); raw_t = (int16_t)(p[0] >> 16);
        if (((texparam >> 30) & 3) == 1) {
            tc_s = (int32_t)(((int64_t)raw_s * tex[0] + (int64_t)raw_t * tex[4] + tex[8] + tex[12]) >> 12);
            tc_t = (int32_t)(((int64_t)raw_s * tex[1] + (int64_t)raw_t * tex[5] + tex[9] + tex[13]) >> 12);
        } else { tc_s = raw_s; tc_t = raw_t; }
        break;
    case 0x23: add_vertex((int16_t)(p[0] & 0xffff), (int16_t)(p[0] >> 16), (int16_t)(p[1] & 0xffff)); break;
    case 0x24: add_vertex(sx10(p[0]) << 6, sx10(p[0] >> 10) << 6, sx10(p[0] >> 20) << 6); break;
    case 0x25: add_vertex((int16_t)(p[0] & 0xffff), (int16_t)(p[0] >> 16), last_z); break;
    case 0x26: add_vertex((int16_t)(p[0] & 0xffff), last_y, (int16_t)(p[0] >> 16)); break;
    case 0x27: add_vertex(last_x, (int16_t)(p[0] & 0xffff), (int16_t)(p[0] >> 16)); break;
    case 0x28: add_vertex((int16_t)(last_x + sx10(p[0])), (int16_t)(last_y + sx10(p[0] >> 10)), (int16_t)(last_z + sx10(p[0] >> 20))); break;
    case 0x29: poly_attr = p[0]; break;
    case 0x2a: texparam = p[0]; break;
    case 0x2b: plttbase = p[0] & 0x1fff; break;

    case 0x30:
        dif = (uint16_t)(p[0] & 0x7fff); amb = (uint16_t)((p[0] >> 16) & 0x7fff);
        if (p[0] & 0x8000) c5to6(dif, &vtx_r, &vtx_g, &vtx_b);
        break;
    case 0x31: spe = (uint16_t)(p[0] & 0x7fff); emi = (uint16_t)((p[0] >> 16) & 0x7fff); use_shininess = (p[0] >> 15) & 1; break;
    case 0x32: {
        int l = (int)(p[0] >> 30);
        int32_t v[3] = { sx10(p[0]), sx10(p[0] >> 10), sx10(p[0] >> 20) };
        for (int i = 0; i < 3; i++)
            light_vec[l][i] = (int32_t)(((int64_t)v[0] * vec[0 * 4 + i] + (int64_t)v[1] * vec[1 * 4 + i] + (int64_t)v[2] * vec[2 * 4 + i]) >> 12);
        break; }
    case 0x33: light_col[p[0] >> 30] = (uint16_t)(p[0] & 0x7fff); break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) shininess[i * 4 + k] = (uint8_t)(p[i] >> (8 * k)); break;

    case 0x40: prim = p[0] & 3; prim_count = 0; poly_attr_cur = poly_attr; break;
    case 0x41: break;   /* END_VTXS: ohne Wirkung */
    case 0x50:          /* SWAP_BUFFERS */
        list_sort[cur_list] = p[0] & 1;
        list_wbuf[cur_list] = (p[0] >> 1) & 1;
        swap_pending = 1;
        break;
    case 0x60: viewport = p[0]; break;
    case 0x70: box_result = 1; flags |= G3D_SIM_BOXTEST; break;
    case 0x71: {
        int32_t x = (int16_t)(p[0] & 0xffff), y = (int16_t)(p[0] >> 16), z = (int16_t)(p[1] & 0xffff);
        last_x = (int16_t)x; last_y = (int16_t)y; last_z = (int16_t)z;
        update_clip();
        for (int i = 0; i < 4; i++)
            pos_result[i] = (int32_t)(((int64_t)x * clip[i] + (int64_t)y * clip[4 + i] + (int64_t)z * clip[8 + i] + 4096ll * clip[12 + i]) >> 12);
        break; }
    case 0x72: {
        int32_t v[3] = { sx10(p[0]), sx10(p[0] >> 10), sx10(p[0] >> 20) };
        for (int i = 0; i < 3; i++)
            vec_result[i] = (int16_t)(((int64_t)v[0] * vec[i] + (int64_t)v[1] * vec[4 + i] + (int64_t)v[2] * vec[8 + i]) >> 12);
        break; }
    default: break;
    }
}

/* gepackte FIFO: Befehlswort mit bis zu 4 Befehlen, danach ihre Parameter */
static uint8_t pk[4];
static int pk_n, pk_i, pk_got;
static uint32_t pk_par[32];

static void pk_run_zero(void) {
    while (pk_i < pk_n && nparams[pk[pk_i]] == 0) exec_cmd(pk[pk_i++], pk_par);
}

static uint8_t port_cmd;
static int port_got;
static uint32_t port_par[32];

void gpu3d_write(uint32_t off, uint32_t v) {
    if (off >= 0x400 && off < 0x440) {
        if (pk_i < pk_n) {
            pk_par[pk_got++] = v;
            if (pk_got >= nparams[pk[pk_i]]) { exec_cmd(pk[pk_i], pk_par); pk_got = 0; pk_i++; pk_run_zero(); }
            return;
        }
        pk_n = pk_i = pk_got = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t c = (uint8_t)(v >> (8 * i));
            if (c) pk[pk_n++] = c;
        }
        pk_run_zero();
        return;
    }
    if (off >= 0x440 && off < 0x600) {
        uint8_t cmd = (uint8_t)(0x10 + (off - 0x440) / 4);
        int np = nparams[cmd];
        if (np == 0) { exec_cmd(cmd, port_par); port_got = 0; return; }
        if (cmd != port_cmd) { port_cmd = cmd; port_got = 0; }
        port_par[port_got++] = v;
        if (port_got >= np) { exec_cmd(cmd, port_par); port_got = 0; }
    }
}

void gpu3d_regs(void) {
    uint32_t st = r32(0x600) & 0xc0000000u;   /* IRQ-Modus behalten */
    st |= (uint32_t)box_result << 1;
    st |= (uint32_t)(pos_sp & 31) << 8;
    st |= (uint32_t)(proj_sp & 1) << 13;
    st |= (uint32_t)stack_err << 15;
    st |= 1u << 25 | 1u << 26;               /* FIFO leer, nie beschäftigt */
    w32(0x600, st);
    int show = pending_valid ? pending_list : cur_list;
    w32(0x604, (npolys[show] & 0xfff) | (nverts[show] & 0x1fff) << 16);
    for (int i = 0; i < 4; i++) w32(0x620 + 4 * (uint32_t)i, (uint32_t)pos_result[i]);
    for (int i = 0; i < 3; i++) w16(0x630 + 2 * (uint32_t)i, (uint16_t)vec_result[i]);
    update_clip();
    for (int i = 0; i < 16; i++) w32(0x640 + 4 * (uint32_t)i, (uint32_t)clip[i]);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) w32(0x680 + 4 * (uint32_t)(r * 3 + c), (uint32_t)vec[r * 4 + c]);
}

void gpu3d_ack_error(void) { stack_err = 0; }

/* ================================================================ Rasterizer */

static Px3D frame[W * H];
static uint8_t fb_r[W * H], fb_g[W * H], fb_b[W * H], fb_a[W * H];
static float zbuf[W * H];
static uint8_t *tex_slot[4], *pal_slot[6];
static unsigned drawn_polys, drawn_verts;

static inline uint8_t tex8(uint32_t a) {
    uint8_t *s = tex_slot[(a >> 17) & 3];
    return s ? s[a & 0x1ffff] : 0;
}
static inline uint16_t tex16(uint32_t a) { return (uint16_t)(tex8(a) | tex8(a + 1) << 8); }
static inline uint16_t pal16(uint32_t a) {
    uint8_t *s = pal_slot[(a >> 14) % 6];
    if (a >= 0x18000 || !s) return 0;
    return (uint16_t)(s[a & 0x3fff] | s[(a + 1) & 0x3fff] << 8);
}

/* Texel holen: liefert Farbe (BGR555) und Alpha 0-31 */
static void tex_fetch(uint32_t tp, uint32_t pltt, int s, int t, uint16_t *col, int *alpha) {
    int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
    if ((tp >> 16) & 1) {
        if ((tp >> 18) & 1) { s &= 2 * w - 1; if (s >= w) s = 2 * w - 1 - s; }
        else s &= w - 1;
    } else s = s < 0 ? 0 : s >= w ? w - 1 : s;
    if ((tp >> 17) & 1) {
        if ((tp >> 19) & 1) { t &= 2 * h - 1; if (t >= h) t = 2 * h - 1 - t; }
        else t &= h - 1;
    } else t = t < 0 ? 0 : t >= h ? h - 1 : t;
    uint32_t addr = (tp & 0xffff) << 3;
    int fmt = (tp >> 26) & 7, c0 = (tp >> 29) & 1;
    uint32_t pb = pltt << 4;
    uint32_t i = (uint32_t)(t * w + s);
    *alpha = 31;
    switch (fmt) {
    case 1: { uint8_t b = tex8(addr + i); int a3 = b >> 5; *col = pal16(pb + (b & 31) * 2u); *alpha = a3 * 4 + a3 / 2; break; }
    case 2: { int ci = (tex8(addr + i / 4) >> ((i & 3) * 2)) & 3; *col = pal16((pltt << 3) + (uint32_t)ci * 2u); if (c0 && !ci) *alpha = 0; break; }
    case 3: { int ci = (tex8(addr + i / 2) >> ((i & 1) * 4)) & 15; *col = pal16(pb + (uint32_t)ci * 2u); if (c0 && !ci) *alpha = 0; break; }
    case 4: { int ci = tex8(addr + i); *col = pal16(pb + (uint32_t)ci * 2u); if (c0 && !ci) *alpha = 0; break; }
    case 5: {   /* 4x4-Blöcke */
        uint32_t bx = (uint32_t)s / 4, by = (uint32_t)t / 4, blk = by * ((uint32_t)w / 4) + bx;
        uint32_t ba = addr + blk * 4;
        int ci = (tex8(ba + (uint32_t)(t & 3)) >> ((s & 3) * 2)) & 3;
        /* Paletteninfo liegt in Slot 1: Texel aus Slot 0 -> erste Hälfte, aus Slot 2 -> zweite */
        uint32_t ia = 0x20000u + (((ba >> 17) & 3) == 2 ? 0x10000u : 0u) + ((ba & 0x1ffff) >> 1);
        uint16_t info = tex16(ia);
        uint32_t pa = pb + (uint32_t)(info & 0x3fff) * 4;
        int mode = info >> 14;
        uint16_t p0 = pal16(pa), p1 = pal16(pa + 2);
        #define MIX(a, b, wa, wb, d) (uint16_t)( \
            ((((a) & 31) * (wa) + ((b) & 31) * (wb)) / (d)) | \
            (((((a) >> 5) & 31) * (wa) + (((b) >> 5) & 31) * (wb)) / (d)) << 5 | \
            (((((a) >> 10) & 31) * (wa) + (((b) >> 10) & 31) * (wb)) / (d)) << 10)
        switch (ci) {
        case 0: *col = p0; break;
        case 1: *col = p1; break;
        case 2:
            if (mode == 1) *col = MIX(p0, p1, 1, 1, 2);
            else if (mode == 3) *col = MIX(p0, p1, 5, 3, 8);
            else *col = pal16(pa + 4);
            break;
        case 3:
            if (mode == 2) *col = pal16(pa + 6);
            else if (mode == 3) *col = MIX(p0, p1, 3, 5, 8);
            else { *col = 0; *alpha = 0; }
            break;
        }
        #undef MIX
        break; }
    case 6: { uint8_t b = tex8(addr + i); *col = pal16(pb + (b & 7) * 2u); *alpha = b >> 3; break; }
    case 7: { uint16_t c = tex16(addr + i * 2); *col = c & 0x7fff; *alpha = (c & 0x8000) ? 31 : 0; break; }
    default: *col = 0x7fff; break;
    }
}

static int cur_wbuf;
static uint32_t disp3d;

static void plot(const Poly *p, int x, int y, float depth, int vr, int vg, int vb, float s, float t) {
    int idx = y * W + x;
    int eq = (p->attr >> 14) & 1;
    if (eq ? fabsf(depth - zbuf[idx]) > 512.0f && depth > zbuf[idx] : depth >= zbuf[idx]) return;
    int av = (p->attr >> 16) & 31;
    if (av == 0) av = 31;   /* Drahtgitter: Kanten deckend */
    int mode = (p->attr >> 4) & 3;
    int r = vr, g = vg, b = vb, a = av;
    if (mode == 2) {   /* Toon / Highlight: Tabelle ab 0x04000380 */
        uint16_t tc = r16(0x380 + (uint32_t)(vr >> 1) * 2);
        int tr, tg, tb;
        c5to6(tc, &tr, &tg, &tb);
        if (!((disp3d >> 1) & 1)) { r = tr; g = tg; b = tb; }
        else { r = vr; g = vg; b = vb; }   /* Highlight: Tabelle wird unten addiert */
    }
    int fmt = (p->texp >> 26) & 7;
    if (fmt && (disp3d & 1)) {
        uint16_t tc; int ta;
        tex_fetch(p->texp, p->pltt, (int)floorf(s / 16.0f), (int)floorf(t / 16.0f), &tc, &ta);
        int tr, tg, tb;
        c5to6(tc, &tr, &tg, &tb);
        if (mode == 1) {   /* Decal */
            if (ta == 31) { r = tr; g = tg; b = tb; }
            else if (ta > 0) { r = (tr * ta + r * (31 - ta)) / 31; g = (tg * ta + g * (31 - ta)) / 31; b = (tb * ta + b * (31 - ta)) / 31; }
        } else {           /* Modulate (auch Toon/Highlight) */
            r = ((tr + 1) * (r + 1) - 1) / 64; g = ((tg + 1) * (g + 1) - 1) / 64; b = ((tb + 1) * (b + 1) - 1) / 64;
            a = ((ta + 1) * (av + 1) - 1) / 32;
        }
    }
    if (mode == 2 && ((disp3d >> 1) & 1)) {
        uint16_t tc = r16(0x380 + (uint32_t)(vr >> 1) * 2);
        int tr, tg, tb;
        c5to6(tc, &tr, &tg, &tb);
        r += tr; g += tg; b += tb;
        if (r > 63) r = 63;
        if (g > 63) g = 63;
        if (b > 63) b = 63;
    }
    if (a <= 0) return;
    if (((disp3d >> 2) & 1) && a <= (int)(r16(0x340) & 31)) return;   /* Alpha-Test */
    if (a >= 31 || !((disp3d >> 3) & 1)) {
        fb_r[idx] = (uint8_t)r; fb_g[idx] = (uint8_t)g; fb_b[idx] = (uint8_t)b;
        fb_a[idx] = (uint8_t)a;
        zbuf[idx] = depth;
    } else {
        int da = fb_a[idx];
        if (da) {
            fb_r[idx] = (uint8_t)((r * (a + 1) + fb_r[idx] * (31 - a)) / 32);
            fb_g[idx] = (uint8_t)((g * (a + 1) + fb_g[idx] * (31 - a)) / 32);
            fb_b[idx] = (uint8_t)((b * (a + 1) + fb_b[idx] * (31 - a)) / 32);
            fb_a[idx] = (uint8_t)(a > da ? a : da);
        } else {
            fb_r[idx] = (uint8_t)r; fb_g[idx] = (uint8_t)g; fb_b[idx] = (uint8_t)b; fb_a[idx] = (uint8_t)a;
        }
        if ((p->attr >> 11) & 1) zbuf[idx] = depth;
    }
}

static float vdepth(const SVtx *v) {
    if (cur_wbuf) return v->w;                                    /* W-Buffer: w (20.12) */
    return (v->z * 0.5f + 0.5f) * (float)0xffffff;                /* Z-Buffer: 24 Bit */
}

static void raster_tri(const Poly *p, const SVtx *a, const SVtx *b, const SVtx *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x))), maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y))), maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > W) maxx = W;
    if (maxy > H) maxy = H;
    const SVtx *v[3] = { a, b, c };
    float iw[3], d[3];
    for (int i = 0; i < 3; i++) { iw[i] = 1.0f / v[i]->w; d[i] = vdepth(v[i]); }
    float sgn = area > 0 ? 1.0f : -1.0f;
    for (int y = miny; y < maxy; y++) {
        float py = (float)y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f;
            float e[3];
            int inside = 1;
            for (int i = 0; i < 3; i++) {
                const SVtx *p0 = v[(i + 1) % 3], *p1 = v[(i + 2) % 3];
                e[i] = ((p1->x - p0->x) * (py - p0->y) - (p1->y - p0->y) * (px - p0->x)) * sgn;
                if (e[i] < 0) { inside = 0; break; }
                if (e[i] == 0) {   /* Kante genau getroffen: nur obere/linke Kanten zählen */
                    float dy = (p1->y - p0->y) * sgn, dx = (p1->x - p0->x) * sgn;
                    if (!(dy < 0 || (dy == 0 && dx > 0))) { inside = 0; break; }
                }
            }
            if (!inside) continue;
            float sum = e[0] + e[1] + e[2];
            float l0 = e[0] / sum, l1 = e[1] / sum, l2 = e[2] / sum;
            float q0 = l0 * iw[0], q1 = l1 * iw[1], q2 = l2 * iw[2];
            float qs = q0 + q1 + q2;
            q0 /= qs; q1 /= qs; q2 /= qs;   /* perspektivisch korrekte Gewichte */
            float depth = cur_wbuf ? 1.0f / (l0 * iw[0] + l1 * iw[1] + l2 * iw[2]) : l0 * d[0] + l1 * d[1] + l2 * d[2];
            int r = (int)(q0 * a->r + q1 * b->r + q2 * c->r + 0.5f);
            int g = (int)(q0 * a->g + q1 * b->g + q2 * c->g + 0.5f);
            int bb = (int)(q0 * a->b + q1 * b->b + q2 * c->b + 0.5f);
            float s = q0 * a->s + q1 * b->s + q2 * c->s, t = q0 * a->t + q1 * b->t + q2 * c->t;
            plot(p, x, y, depth, r, g, bb, s, t);
        }
    }
}

static void raster_line(const Poly *p, const SVtx *a, const SVtx *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        int x = (int)(a->x + dx * t), y = (int)(a->y + dy * t);
        if (x < 0 || x >= W || y < 0 || y >= H) continue;
        SVtx v = *a;
        float depth = vdepth(a) + (vdepth(b) - vdepth(a)) * t;
        plot(p, x, y, depth, (int)(a->r + (b->r - a->r) * t), (int)(a->g + (b->g - a->g) * t),
             (int)(a->b + (b->b - a->b) * t), v.s + (b->s - v.s) * t, v.t + (b->t - v.t) * t);
    }
}

static void raster_poly(const Poly *p) {
    if (((p->attr >> 4) & 3) == 3) { flags |= G3D_MISS_SHADOW; return; }
    if (((p->attr >> 16) & 31) == 0) {   /* Drahtgitter */
        for (int i = 0; i < p->n; i++) raster_line(p, &p->v[i], &p->v[(i + 1) % p->n]);
        return;
    }
    for (int i = 1; i + 1 < p->n; i++) raster_tri(p, &p->v[0], &p->v[i], &p->v[i + 1]);
}

static int cmp_ty(const void *x, const void *y) {
    const Poly *a = *(const Poly *const *)x, *b = *(const Poly *const *)y;
    if (a->ymax != b->ymax) return a->ymax < b->ymax ? -1 : 1;
    if (a->ymin != b->ymin) return a->ymin < b->ymin ? -1 : 1;
    return a < b ? -1 : a > b;   /* stabil: Eingabereihenfolge */
}

#include <stdlib.h>

static void render_list(int L) {
    disp3d = r16(0x060);
    if (disp3d & (1u << 4)) flags |= G3D_MISS_EDGE;
    if (disp3d & (1u << 7)) flags |= G3D_MISS_FOG;
    if (disp3d & (1u << 5)) flags |= G3D_MISS_AA;
    if (disp3d & (1u << 14)) flags |= G3D_MISS_REARBMP;
    vram_tex_slots(tex_slot, pal_slot);
    if (getenv("BIS_3DTEX") && npolys[L] && gx_vblanks >= (unsigned)atoi(getenv("BIS_3DTEX"))) {   /* Fehlersuche: Textur-Slots und erste Texturbytes ab VBlank n */
        static int shown;
        if (shown++ < 3) {
            fprintf(stderr, "  [3dtex] Slots tex %d%d%d%d pal %d%d%d%d%d%d\n", !!tex_slot[0], !!tex_slot[1], !!tex_slot[2],
                    !!tex_slot[3], !!pal_slot[0], !!pal_slot[1], !!pal_slot[2], !!pal_slot[3], !!pal_slot[4], !!pal_slot[5]);
            for (unsigned i = 0; i < npolys[L] && i < 6; i++) {
                uint32_t tp = polys[L][i].texp, a = (tp & 0xffff) << 3;
                unsigned nz = 0;
                for (uint32_t k = 0; k < 2048; k++) nz += tex8(a + k) != 0;
                fprintf(stderr, "  [3dtex] poly %u tex %08x addr %05x fmt %u pltt %04x: %u/2048 Bytes ungleich 0, Palette[0..3] %04x %04x %04x %04x\n",
                        i, tp, a, (tp >> 26) & 7, polys[L][i].pltt, nz, pal16(polys[L][i].pltt << 4), pal16((polys[L][i].pltt << 4) + 2),
                        pal16((polys[L][i].pltt << 4) + 4), pal16((polys[L][i].pltt << 4) + 6));
            }
        }
    }
    cur_wbuf = list_wbuf[L];

    uint32_t cc = r32(0x350);
    int cr, cg, cb;
    c5to6((uint16_t)(cc & 0x7fff), &cr, &cg, &cb);
    uint8_t ca = (uint8_t)((cc >> 16) & 31);
    uint32_t cd = r16(0x354) & 0x7fff;
    float clear_depth = cur_wbuf ? (float)(cd * 0x200 + (cd == 0x7fff ? 0x1ff : 0)) : (float)(cd * 0x200 + (cd == 0x7fff ? 0x1ff : 0));
    for (int i = 0; i < W * H; i++) {
        fb_r[i] = (uint8_t)cr; fb_g[i] = (uint8_t)cg; fb_b[i] = (uint8_t)cb; fb_a[i] = ca; zbuf[i] = clear_depth;
    }

    static const Poly *order[MAX_POLYS];
    unsigned n = 0, nt;
    for (unsigned i = 0; i < npolys[L]; i++) if (!polys[L][i].translucent) order[n++] = &polys[L][i];
    qsort(order, n, sizeof order[0], cmp_ty);
    nt = n;
    for (unsigned i = 0; i < npolys[L]; i++) if (polys[L][i].translucent) order[n++] = &polys[L][i];
    if (!list_sort[L]) qsort(order + nt, n - nt, sizeof order[0], cmp_ty);
    for (unsigned i = 0; i < n; i++) raster_poly(order[i]);

    for (int i = 0; i < W * H; i++) {
        frame[i].color = (uint16_t)((fb_r[i] >> 1) | (fb_g[i] >> 1) << 5 | (fb_b[i] >> 1) << 10);
        frame[i].alpha = fb_a[i];
    }
    drawn_polys = npolys[L];
    drawn_verts = nverts[L];
}

void gpu3d_vblank(void) {
    gx_vblanks++;
    /* Liste aus dem vorigen Frame zeichnen (die Hardware rendert sie während dieses Frames) */
    static int skip_render = -1;   /* Messung: BIS_NO3DRENDER=1 ueberspringt das Zeichnen */
    if (skip_render < 0) skip_render = getenv("BIS_NO3DRENDER") != NULL;
    if (pending_valid) { if (!skip_render) render_list(pending_list); pending_valid = 0; }
    if (swap_pending) {
        pending_list = cur_list;
        pending_valid = 1;
        swap_pending = 0;
        cur_list ^= 1;
        npolys[cur_list] = nverts[cur_list] = 0;
    }
}

const Px3D *gpu3d_frame(void) { return frame; }
unsigned gpu3d_flags(void) { return flags; }
void gpu3d_stats(unsigned *p, unsigned *v, uint64_t *c) { *p = drawn_polys; *v = drawn_verts; *c = cmd_count; }

void gpu3d_debug(void) {
    fprintf(stderr, "  [3d] Polygone gesamt %u: ferne Ebene %u, weggeclippt %u, Rueckseite %u\n",
            dbg_in, dbg_far, dbg_clip, dbg_cull);
    int L = cur_list ^ 1;
    for (unsigned i = 0; i < npolys[L] && i < 8; i++) {
        const Poly *p = &polys[L][i];
        fprintf(stderr, "  [3d] Liste %u: attr %08x tex %08x pltt %04x n=%d", i, p->attr, p->texp, p->pltt, p->n);
        for (int k = 0; k < p->n; k++) fprintf(stderr, " (%.1f,%.1f z%.3f w%.0f)", p->v[k].x, p->v[k].y, p->v[k].z, p->v[k].w);
        fprintf(stderr, "\n");
    }
}

void gpu3d_init(uint8_t *iop) {
    io = iop;
    mtx_ident(proj); mtx_ident(pos); mtx_ident(vec); mtx_ident(tex);
    clip_dirty = 1;
    memset(frame, 0, sizeof frame);
    (void)pending_viewport_dummy; (void)pending_sort; (void)pending_wbuf; (void)tex_sp;
}
