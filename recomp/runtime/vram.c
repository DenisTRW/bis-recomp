/* VRAM-Bänke des DS (ARM9-Sicht), nach GBATEK "DS Memory Control - VRAM".
 *
 *   Bank  Größe   MST 0 (LCDC)   MST 1        MST 2          MST 3      MST 4/5
 *   A,B   128K    0x06800000+    A-BG ofs     A-OBJ ofs&1    Textur     -
 *   C     128K    0x06840000     A-BG ofs     (ARM7)         Textur     B-BG 0x06200000
 *   D     128K    0x06860000     A-BG ofs     (ARM7)         Textur     B-OBJ 0x06600000
 *   E     64K     0x06880000     A-BG         A-OBJ          Tex-Pal    A-BG-ExtPal
 *   F,G   16K     0x06890000/94  A-BG ofs     A-OBJ ofs      Tex-Pal    4: A-BG-ExtPal, 5: A-OBJ-ExtPal
 *   H     32K     0x06898000     B-BG         B-BG-ExtPal    -          -
 *   I     16K     0x068a0000     B-BG +32K    B-OBJ          B-OBJ-ExtPal
 * Überlappen sich mehrere Bänke, wird die erste genommen (auf echter Hardware
 * werden die Werte verodert – kommt in korrekten Spielen nicht vor).
 */
#include <string.h>
#include "vram.h"

enum { R_NONE, R_LCDC, R_A_BG, R_A_OBJ, R_B_BG, R_B_OBJ, R_A_BGEXT, R_A_OBJEXT, R_B_BGEXT, R_B_OBJEXT, R_TEX, R_TEXPAL, R_ARM7 };

static const uint32_t bank_size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
static const uint32_t lcdc_base[9] = { 0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                       0x06890000, 0x06894000, 0x06898000, 0x068a0000 };
static uint8_t mem_a[0x20000], mem_b[0x20000], mem_c[0x20000], mem_d[0x20000];
static uint8_t mem_e[0x10000], mem_f[0x4000], mem_g[0x4000], mem_h[0x8000], mem_i[0x4000];
static uint8_t *const bank_mem[9] = { mem_a, mem_b, mem_c, mem_d, mem_e, mem_f, mem_g, mem_h, mem_i };

static struct { int region; uint32_t base; } map[9];   /* base = Offset im Zielbereich */

void vram_set_cnt(int bank, uint8_t v) {
    if (bank < 0 || bank > 8) return;
    map[bank].region = R_NONE;
    map[bank].base = 0;
    if (!(v & 0x80)) return;
    int mst = v & 7, ofs = (v >> 3) & 3;
    switch (bank) {
    case 0: case 1: case 2: case 3:
        mst &= 7;
        if (mst == 0) map[bank].region = R_LCDC;
        else if (mst == 1) { map[bank].region = R_A_BG; map[bank].base = 0x20000u * (uint32_t)ofs; }
        else if (mst == 2 && bank < 2) { map[bank].region = R_A_OBJ; map[bank].base = 0x20000u * (uint32_t)(ofs & 1); }
        else if (mst == 2) map[bank].region = R_ARM7;
        else if (mst == 3) { map[bank].region = R_TEX; map[bank].base = 0x20000u * (uint32_t)ofs; }
        else if (mst == 4 && bank == 2) map[bank].region = R_B_BG;
        else if (mst == 4 && bank == 3) map[bank].region = R_B_OBJ;
        break;
    case 4:
        if (mst == 0) map[bank].region = R_LCDC;
        else if (mst == 1) map[bank].region = R_A_BG;
        else if (mst == 2) map[bank].region = R_A_OBJ;
        else if (mst == 3) map[bank].region = R_TEXPAL;
        else if (mst == 4) map[bank].region = R_A_BGEXT;
        break;
    case 5: case 6: {
        uint32_t o = (uint32_t)(ofs & 1) * 0x4000u + (uint32_t)(ofs >> 1) * 0x10000u;
        if (mst == 0) map[bank].region = R_LCDC;
        else if (mst == 1) { map[bank].region = R_A_BG; map[bank].base = o; }
        else if (mst == 2) { map[bank].region = R_A_OBJ; map[bank].base = o; }
        else if (mst == 3) { map[bank].region = R_TEXPAL; map[bank].base = (uint32_t)((ofs & 1) + 4 * (ofs >> 1)) * 0x4000u; }
        else if (mst == 4) { map[bank].region = R_A_BGEXT; map[bank].base = (uint32_t)(ofs & 1) * 0x4000u; }
        else if (mst == 5) map[bank].region = R_A_OBJEXT;
        break;
    }
    case 7:
        if (mst == 0) map[bank].region = R_LCDC;
        else if (mst == 1) map[bank].region = R_B_BG;
        else if (mst == 2) map[bank].region = R_B_BGEXT;
        break;
    case 8:
        if (mst == 0) map[bank].region = R_LCDC;
        else if (mst == 1) { map[bank].region = R_B_BG; map[bank].base = 0x8000; }
        else if (mst == 2) map[bank].region = R_B_OBJ;
        else if (mst == 3) map[bank].region = R_B_OBJEXT;
        break;
    }
}

/* Byte im Bereich `region` bei Offset `off` (über alle passenden Bänke) */
static uint8_t *region_ptr(int region, uint32_t off) {
    for (int b = 0; b < 9; b++) {
        if (map[b].region != region) continue;
        if (off >= map[b].base && off < map[b].base + bank_size[b]) return &bank_mem[b][off - map[b].base];
    }
    return NULL;
}

uint8_t *vram_cpu_ptr(uint32_t addr) {
    if (addr >= 0x06800000u && addr < 0x068a4000u) {
        for (int b = 0; b < 9; b++)
            if (map[b].region == R_LCDC && addr >= lcdc_base[b] && addr < lcdc_base[b] + bank_size[b])
                return &bank_mem[b][addr - lcdc_base[b]];
        return NULL;
    }
    switch ((addr >> 21) & 7) {                       /* Bereiche je 2 MB, gespiegelt */
    case 0: return region_ptr(R_A_BG, addr & 0x7ffff);
    case 1: return region_ptr(R_B_BG, addr & 0x1ffff);
    case 2: return region_ptr(R_A_OBJ, addr & 0x3ffff);
    case 3: return region_ptr(R_B_OBJ, addr & 0x1ffff);
    default: return NULL;
    }
}

uint8_t vram_bg_read8(int eng, uint32_t off) {
    uint8_t *p = region_ptr(eng ? R_B_BG : R_A_BG, off & (eng ? 0x1ffff : 0x7ffff));
    return p ? *p : 0;
}

uint8_t vram_obj_read8(int eng, uint32_t off) {
    uint8_t *p = region_ptr(eng ? R_B_OBJ : R_A_OBJ, off & (eng ? 0x1ffff : 0x3ffff));
    return p ? *p : 0;
}

uint16_t vram_bg_extpal(int eng, int slot, uint32_t i) {
    uint32_t off = (uint32_t)slot * 0x2000u + (i & 0xfff) * 2;
    uint8_t *p = region_ptr(eng ? R_B_BGEXT : R_A_BGEXT, off);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}

uint16_t vram_obj_extpal(int eng, uint32_t i) {
    uint32_t off = (i & 0xfff) * 2;
    uint8_t *p = region_ptr(eng ? R_B_OBJEXT : R_A_OBJEXT, off);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}
