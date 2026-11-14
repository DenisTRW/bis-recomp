/* 2D-Renderer (Software), nach GBATEK "DS Video".
 *
 * Unterstützt (Stand erstes Bild):
 *   - Anzeigemodus 1 (Grafik), erzwungenes Leerbild
 *   - Text-Hintergründe (alle BG-Modi, soweit die Ebene Text ist), 16/256 Farben,
 *     erweiterte BG-Paletten, Größen 256/512, Scrolling, Spiegeln
 *   - normale Sprites (nicht affin), 16/256 Farben, 1D- und 2D-Abbildung,
 *     erweiterte Sprite-Paletten, Prioritäten
 *   - Master-Helligkeit (Ein-/Ausblenden)
 *   - 3D-Ebene als BG0 von Engine A (Bild aus gpu3d.c, mit 3D-Alpha)
 *   - Farbeffekte (BLDCNT): Alpha-Blending, Aufhellen/Abdunkeln, halbtransparente Sprites
 * Nicht unterstützt (wird in `missing` gemeldet): affine/erweiterte BGs,
 * affine und Bitmap-Sprites, Fenster, Mosaik, Anzeigemodi 2/3.
 * Gezeichnet wird der Zustand am Ende eines Frames, nicht Zeile für Zeile.
 */
#include <string.h>
#include "render2d.h"
#include "vram.h"

#define W 256
#define H 192

static inline uint16_t rd16io(const uint8_t *io, uint32_t off) { return (uint16_t)(io[off] | io[off + 1] << 8); }
static inline uint32_t rd32io(const uint8_t *io, uint32_t off) { return rd16io(io, off) | (uint32_t)rd16io(io, off + 2) << 16; }

static inline uint32_t to_rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* Ein Pixel einer Ebene: Farbe (BGR555) + Gültigkeit */
typedef struct { uint16_t color; uint8_t valid, prio, a3d, semi; } Px;   /* a3d: Alpha der 3D-Ebene, semi: halbtransparentes Sprite */

/* ---------------------------------------------------------------- Text-Hintergrund */

static void draw_text_bg(const Gfx2D *g, int eng, int n, uint32_t dispcnt, Px *layer) {
    const uint8_t *io = g->io + (eng ? 0x1000 : 0);
    uint16_t cnt = rd16io(io, 0x08 + 2 * (uint32_t)n);
    uint32_t hofs = rd16io(io, 0x10 + 4 * (uint32_t)n) & 0x1ff, vofs = rd16io(io, 0x12 + 4 * (uint32_t)n) & 0x1ff;
    uint32_t char_base = ((cnt >> 2) & 15) * 0x4000u, screen_base = ((cnt >> 8) & 31) * 0x800u;
    if (!eng) {
        char_base += ((dispcnt >> 24) & 7) * 0x10000u;
        screen_base += ((dispcnt >> 27) & 7) * 0x10000u;
    }
    int c256 = (cnt >> 7) & 1, size = (cnt >> 14) & 3;
    uint32_t wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    int ext = c256 && ((dispcnt >> 30) & 1);
    int slot = n;
    if (n < 2 && ((cnt >> 13) & 1)) slot = n + 2;
    const uint8_t *pal = g->pal + (eng ? 0x400 : 0);

    for (uint32_t y = 0; y < H; y++) {
        uint32_t sy = (y + vofs) & hmask;
        for (uint32_t x = 0; x < W; x++) {
            uint32_t sx = (x + hofs) & wmask;
            /* Bildschirmblöcke zu 32x32 Kacheln; 512 breit: rechter Block folgt */
            uint32_t block = 0;
            if (sx >= 256) block += 1;
            if (sy >= 256) block += (size == 3) ? 2 : 1;
            uint32_t ent_off = screen_base + block * 0x800u + ((sy & 255) / 8 * 32 + (sx & 255) / 8) * 2;
            uint16_t ent = (uint16_t)(vram_bg_read8(eng, ent_off) | vram_bg_read8(eng, ent_off + 1) << 8);
            uint32_t tile = ent & 0x3ff, tx = sx & 7, ty = sy & 7;
            if (ent & 0x400) tx = 7 - tx;
            if (ent & 0x800) ty = 7 - ty;
            uint32_t palnum = ent >> 12;
            uint32_t ci;
            if (c256) {
                ci = vram_bg_read8(eng, char_base + tile * 64 + ty * 8 + tx);
            } else {
                uint8_t b = vram_bg_read8(eng, char_base + tile * 32 + ty * 4 + tx / 2);
                ci = (tx & 1) ? b >> 4 : b & 15;
            }
            if (!ci) continue;
            uint16_t color;
            if (ext) color = vram_bg_extpal(eng, slot, palnum * 256 + ci);
            else if (c256) color = (uint16_t)(pal[ci * 2] | pal[ci * 2 + 1] << 8);
            else { uint32_t i = palnum * 16 + ci; color = (uint16_t)(pal[i * 2] | pal[i * 2 + 1] << 8); }
            Px *p = &layer[y * W + x];
            p->color = color; p->valid = 1; p->prio = cnt & 3;
        }
    }
}

/* ---------------------------------------------------------------- Sprites */

static const uint8_t obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void draw_objs(const Gfx2D *g, int eng, uint32_t dispcnt, Px *layer, unsigned *missing) {
    const uint8_t *oam = g->oam + (eng ? 0x400 : 0);
    const uint8_t *pal = g->pal + (eng ? 0x600 : 0x200);
    int map1d = (dispcnt >> 4) & 1;
    uint32_t boundary = 32u << ((dispcnt >> 20) & 3);
    int ext = (dispcnt >> 31) & 1;
    /* Sprite 0 liegt vorne: von hinten nach vorne zeichnen */
    for (int i = 127; i >= 0; i--) {
        const uint8_t *o = oam + i * 8;
        uint16_t a0 = (uint16_t)(o[0] | o[1] << 8), a1 = (uint16_t)(o[2] | o[3] << 8), a2 = (uint16_t)(o[4] | o[5] << 8);
        int affine = (a0 >> 8) & 1;
        if (!affine && ((a0 >> 9) & 1)) continue;          /* ausgeblendet */
        if (affine) { *missing |= R2D_MISS_AFFINE_OBJ; continue; }
        int mode = (a0 >> 10) & 3;
        if (mode == 3) { *missing |= R2D_MISS_BITMAP_OBJ; continue; }
        if (mode == 2) { *missing |= R2D_MISS_WINDOW; continue; }
        int shape = (a0 >> 14) & 3, sz = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = obj_w[shape][sz], h = obj_h[shape][sz];
        int y0 = a0 & 0xff, x0 = a1 & 0x1ff;
        if (y0 >= 192) y0 -= 256;
        if (x0 >= 256) x0 -= 512;
        int c256 = (a0 >> 13) & 1, hflip = (a1 >> 12) & 1, vflip = (a1 >> 13) & 1;
        uint32_t tile = a2 & 0x3ff, prio = (a2 >> 10) & 3, palnum = a2 >> 12;
        for (int py = 0; py < h; py++) {
            int sy = y0 + py;
            if (sy < 0 || sy >= H) continue;
            int ty = vflip ? h - 1 - py : py;
            for (int px = 0; px < w; px++) {
                int sx = x0 + px;
                if (sx < 0 || sx >= W) continue;
                int tx = hflip ? w - 1 - px : px;
                uint32_t tiles_x = (uint32_t)w / 8, tn = (uint32_t)(ty / 8) * tiles_x + (uint32_t)(tx / 8);
                uint32_t ci, base;
                if (map1d) {
                    base = tile * boundary + tn * (c256 ? 64u : 32u);
                } else {
                    /* 2D: 32 Kacheln (4-Bit) pro Zeile */
                    uint32_t t = tile + (uint32_t)(ty / 8) * 32u + (uint32_t)(tx / 8) * (c256 ? 2u : 1u);
                    base = t * 32u;
                }
                if (c256) ci = vram_obj_read8(eng, base + (uint32_t)(ty & 7) * 8 + (uint32_t)(tx & 7));
                else {
                    uint8_t b = vram_obj_read8(eng, base + (uint32_t)(ty & 7) * 4 + (uint32_t)(tx & 7) / 2);
                    ci = (tx & 1) ? b >> 4 : b & 15;
                }
                if (!ci) continue;
                uint16_t color;
                if (c256 && ext) color = vram_obj_extpal(eng, palnum * 256 + ci);
                else if (c256) color = (uint16_t)(pal[ci * 2] | pal[ci * 2 + 1] << 8);
                else { uint32_t k = palnum * 16 + ci; color = (uint16_t)(pal[k * 2] | pal[k * 2 + 1] << 8); }
                Px *p = &layer[sy * W + sx];
                p->color = color; p->valid = 1; p->prio = (uint8_t)prio; p->semi = mode == 1;
            }
        }
    }
}

/* ---------------------------------------------------------------- Zusammensetzen */

static Px layers[5][W * H];   /* BG0..BG3, OBJ */

/* Fehlersuche: Bit n = Ebene n zeichnen (BG0..BG3, Bit 4 = Sprites) */
unsigned render2d_layer_mask = 0x1f;

void render2d_engine(const Gfx2D *g, int eng, uint32_t *out, int apply_bright, unsigned *missing) {
    const uint8_t *io = g->io + (eng ? 0x1000 : 0);
    uint32_t dispcnt = rd32io(io, 0);
    uint32_t dmode = (dispcnt >> 16) & 3;
    const uint8_t *pal = g->pal + (eng ? 0x400 : 0);
    uint16_t backdrop = (uint16_t)(pal[0] | pal[1] << 8);

    if (dmode == 0) { for (int i = 0; i < W * H; i++) out[i] = 0xffffff; return; }   /* Anzeige aus = weiß */
    if (dmode != 1) { *missing |= R2D_MISS_DISPMODE; memset(out, 0, W * H * 4); return; }
    if (dispcnt & 0x80) { for (int i = 0; i < W * H; i++) out[i] = 0xffffff; return; } /* erzwungen leer */
    if (dispcnt & 0xe000) *missing |= R2D_MISS_WINDOW;

    memset(layers, 0, sizeof layers);
    int bgmode = dispcnt & 7;
    for (int n = 0; n < 4; n++) {
        if (!((dispcnt >> (8 + n)) & 1) || !((render2d_layer_mask >> n) & 1)) continue;
        if (n == 0 && !eng && (dispcnt & 8)) {   /* BG0 = 3D-Bild, horizontal verschiebbar */
            if (!g->d3) { *missing |= R2D_MISS_3D; continue; }
            int hofs = (int)(rd16io(io, 0x10) << 23) >> 23;
            uint8_t prio = rd16io(io, 0x08) & 3;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int sx = x + hofs;
                    if (sx < 0 || sx >= W) continue;
                    const Px3D *s = &g->d3[y * W + sx];
                    if (!s->alpha) continue;
                    Px *p = &layers[0][y * W + x];
                    p->color = s->color; p->valid = 1; p->prio = prio; p->a3d = s->alpha;
                }
            continue;
        }
        int text = n < 2 || (n == 2 && (bgmode == 0 || bgmode == 1 || bgmode == 3)) || (n == 3 && bgmode == 0);
        if (!text) { *missing |= R2D_MISS_AFFINE_BG; continue; }
        if (rd16io(io, 0x08 + 2 * (uint32_t)n) & 0x40) *missing |= R2D_MISS_MOSAIC;
        draw_text_bg(g, eng, n, dispcnt, layers[n]);
    }
    if (((dispcnt >> 12) & 1) && (render2d_layer_mask & 0x10)) draw_objs(g, eng, dispcnt, layers[4], missing);
    /* Farbeffekte (BLDCNT): Bits 0-5 erstes Ziel (BG0-3, OBJ, Hintergrund), Bits 8-13 zweites */
    uint16_t bld = rd16io(io, 0x50), bla = rd16io(io, 0x52);
    int bmode = (bld >> 6) & 3;
    int eva = bla & 31, evb = (bla >> 8) & 31, evy = rd16io(io, 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    /* Pro Pixel: kleinste Priorität gewinnt, bei Gleichstand Sprite vor BG, BG0 vor BG3.
     * Für das Blending zählen die oberste und die zweitoberste Ebene (5 = Hintergrundfarbe). */
    uint16_t mb = rd16io(io, 0x6c);
    int bright_mode = (mb >> 14) & 3, factor = mb & 31;
    if (factor > 16) factor = 16;
    for (int i = 0; i < W * H; i++) {
        int top = 5, sec = 5, rtop = 1000, rsec = 1000;
        for (int n = 0; n < 5; n++) {
            const Px *p = &layers[n][i];
            if (!p->valid) continue;
            int rank = p->prio * 8 + (n == 4 ? 0 : 1 + n);
            if (rank < rtop) { sec = top; rsec = rtop; top = n; rtop = rank; }
            else if (rank < rsec) { sec = n; rsec = rank; }
        }
        uint16_t c1 = top == 5 ? backdrop : layers[top][i].color;
        uint16_t c2 = sec == 5 ? backdrop : layers[sec][i].color;
        int t1 = (bld >> top) & 1, t2 = (bld >> (8 + sec)) & 1;
        int ea = -1, eb = 0;   /* Gewichte für Alpha-Blending (von 16) */
        if (top == 4 && layers[4][i].semi && t2) { ea = eva; eb = evb; }
        else if (top == 0 && layers[0][i].a3d && layers[0][i].a3d < 31 && t2) {
            ea = layers[0][i].a3d + 1; eb = 32 - ea; ea /= 2; eb /= 2;   /* 3D-Alpha statt EVA/EVB */
        }
        else if (bmode == 1 && t1 && t2) { ea = eva; eb = evb; }
        uint16_t c = c1;
        if (ea >= 0) {
            int r = ((c1 & 31) * ea + (c2 & 31) * eb) / 16, gg = (((c1 >> 5) & 31) * ea + ((c2 >> 5) & 31) * eb) / 16,
                b = (((c1 >> 10) & 31) * ea + ((c2 >> 10) & 31) * eb) / 16;
            c = (uint16_t)((r > 31 ? 31 : r) | (gg > 31 ? 31 : gg) << 5 | (b > 31 ? 31 : b) << 10);
        } else if ((bmode == 2 || bmode == 3) && t1) {
            int ch[3] = { c1 & 31, (c1 >> 5) & 31, (c1 >> 10) & 31 };
            for (int k = 0; k < 3; k++) ch[k] += bmode == 2 ? (31 - ch[k]) * evy / 16 : -(ch[k] * evy / 16);
            c = (uint16_t)(ch[0] | ch[1] << 5 | ch[2] << 10);
        }
        uint32_t rgb = to_rgb(c);
        if (apply_bright && bright_mode && factor) {
            uint32_t r = rgb >> 16 & 255, gg = rgb >> 8 & 255, b = rgb & 255;
            if (bright_mode == 1) { r += (255 - r) * (uint32_t)factor / 16; gg += (255 - gg) * (uint32_t)factor / 16; b += (255 - b) * (uint32_t)factor / 16; }
            else if (bright_mode == 2) { r -= r * (uint32_t)factor / 16; gg -= gg * (uint32_t)factor / 16; b -= b * (uint32_t)factor / 16; }
            rgb = r << 16 | gg << 8 | b;
        }
        out[i] = rgb;
    }
}
