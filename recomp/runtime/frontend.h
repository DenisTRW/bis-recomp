/* Fenster-Frontend (SDL3): Bild zeigen, Tastatur lesen, Bildrate halten */
#pragma once
#include <stdint.h>

int frontend_init(void);                                         /* 1 = ok */
void frontend_present(const uint32_t *top, const uint32_t *bottom);   /* je 256x192, 0x00RRGGBB */
/* Liest Tastatur: keys = KEYINPUT-Bits (aktiv-niedrig), xy = Bits 10 (X) / 11 (Y).
 * Rückgabe 0 = Fenster geschlossen. */
int frontend_poll(uint16_t *keys, uint16_t *xy);
void frontend_pace(void);                                        /* auf 60 Bilder/s bremsen */
/* Lauf beendet (z. B. Absturz): letztes Bild stehen lassen, Grund im Titel, bis das Fenster zu ist */
void frontend_wait_close(const char *reason);
