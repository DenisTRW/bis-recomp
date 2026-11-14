/* VRAM des ARM9: 9 Bänke (A-I), zugeordnet über VRAMCNT (0x04000240-0x04000249). */
#pragma once
#include <stdint.h>

/* Muss nach jedem Schreiben auf VRAMCNT aufgerufen werden */
void vram_set_cnt(int bank, uint8_t value);

/* CPU-Zugriff auf 0x06xxxxxx: Zeiger auf das Byte oder NULL (nicht gemappt) */
uint8_t *vram_cpu_ptr(uint32_t addr);

/* Sicht der Grafik-Engines (eng 0 = A, 1 = B) */
uint8_t vram_bg_read8(int eng, uint32_t off);           /* BG-Speicher, Offset ab Anfang */
uint8_t vram_obj_read8(int eng, uint32_t off);          /* Sprite-Speicher */
uint16_t vram_bg_extpal(int eng, int slot, uint32_t i); /* erweiterte BG-Palette: Slot 0-3, Farbe i */
uint16_t vram_obj_extpal(int eng, uint32_t i);          /* erweiterte Sprite-Palette */
