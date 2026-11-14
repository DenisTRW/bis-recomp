/* 2D-Renderer für eine Grafik-Engine des DS (A = oben/unten je nach POWCNT1) */
#pragma once
#include <stdint.h>
#include "gpu3d.h"

typedef struct {
    const uint8_t *io;    /* I/O-Register ab 0x04000000 (0x2000 Bytes, Engine B ab +0x1000) */
    const uint8_t *pal;   /* Paletten 0x05000000 (2 KB) */
    const uint8_t *oam;   /* OAM 0x07000000 (2 KB) */
    const Px3D *d3;       /* 3D-Bild für BG0 von Engine A (NULL = keins) */
} Gfx2D;

/* Was beim Zeichnen übersprungen wurde (für den HLE-Bericht) */
enum {
    R2D_MISS_AFFINE_BG = 1, R2D_MISS_3D = 2, R2D_MISS_AFFINE_OBJ = 4, R2D_MISS_BITMAP_OBJ = 8,
    R2D_MISS_WINDOW = 16, R2D_MISS_BLEND = 32, R2D_MISS_DISPMODE = 64, R2D_MISS_MOSAIC = 128,
};

/* Zeichnet Engine eng (0 = A, 1 = B) nach out (256*192 Pixel, 0x00RRGGBB).
 * apply_bright: Master-Helligkeit anwenden (0 = Rohbild zur Fehlersuche). */
void render2d_engine(const Gfx2D *g, int eng, uint32_t *out, int apply_bright, unsigned *missing);

/* Fehlersuche: welche Ebenen gezeichnet werden (Bit 0-3 = BG0-BG3, Bit 4 = Sprites) */
extern unsigned render2d_layer_mask;
