/* ARM7-Ersatz (siehe arm7_hle.c) */
#pragma once
#include <stdint.h>

void arm7_pxi_send(uint32_t word);   /* ARM9 schreibt in die Sende-FIFO (0x04000188) */
int arm7_recv_empty(void);           /* Empfangs-FIFO leer? */
uint32_t arm7_recv_pop(void);        /* ARM9 liest 0x04100000 */

/* Spielstand: Datei (optional) für den 8-KB-Spielstandspeicher der Karte.
 * Ohne Datei verhält sich der Speicher wie ein leerer Baustein (0xFF). */
void arm7_set_save_path(const char *path);
void arm7_flush_save(void);
