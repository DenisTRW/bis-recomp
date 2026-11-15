/* 3D-Engine des DS (Software), nach GBATEK "DS 3D Video".
 *
 * Geometrie: GX-Befehle über die gepackte FIFO (0x04000400) oder die direkten
 * Befehlsports (0x04000440-0x040005ff), Matrix-Stapel, Vertices, Polygone, Licht.
 * Rendering: Software-Rasterizer (Z-/W-Buffer, Texturen, Alpha), das Ergebnis wird
 * von render2d als BG0 von Engine A eingeblendet.
 *
 * Zeitverhalten wie auf der Hardware: Die bis SWAP_BUFFERS gesammelte Liste wird zum
 * nächsten VBlank übernommen und im folgenden Frame gezeichnet (eine Frame Verzögerung).
 */
#pragma once
#include <stdint.h>

/* Ein Pixel des 3D-Bildes: Farbe BGR555, Alpha 0-31 (0 = leer/durchsichtig) */
typedef struct { uint16_t color; uint8_t alpha; } Px3D;

void gpu3d_init(uint8_t *io);                 /* io = I/O-Register ab 0x04000000 */
void gpu3d_write(uint32_t off, uint32_t v);   /* 32-Bit-Schreibzugriff auf 0x400-0x5ff */
void gpu3d_regs(void);                        /* GXSTAT, RAM_COUNT, Testergebnisse nach io[] */
void gpu3d_ack_error(void);                   /* GXSTAT Bit 15 geschrieben: Stapelfehler löschen */
unsigned gpu3d_fifo_level(void);              /* Einträge im GX-FIFO (nach SWAP_BUFFERS bis zum VBlank) */
void gpu3d_vblank(void);                      /* VBlank: Liste zeichnen / übernehmen */
const Px3D *gpu3d_frame(void);                /* 256x192 */

/* Was benutzt, aber nicht (vollständig) nachgebildet wurde (für den HLE-Bericht) */
enum {
    G3D_MISS_SHADOW = 1, G3D_MISS_FOG = 2, G3D_MISS_EDGE = 4, G3D_MISS_AA = 8,
    G3D_MISS_TEXGEN = 16, G3D_MISS_REARBMP = 32, G3D_SIM_BOXTEST = 64, G3D_MISS_OVERFLOW = 128,
};
unsigned gpu3d_flags(void);

/* Statistik des zuletzt gezeichneten Bildes */
void gpu3d_stats(unsigned *polys, unsigned *verts, uint64_t *cmds);
void gpu3d_debug(void);   /* Fehlersuche: Zähler und die ersten Polygone nach stderr */
