#pragma once
#include <stdint.h>

/* Schreibt ein RGB-Bild (Pixel als 0x00RRGGBB) als PNG. Rückgabe 1 = ok. */
int png_write_rgb(const char *path, int w, int h, const uint32_t *pix);
