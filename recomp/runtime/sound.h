/* Sound-Treiber des ARM7 (Ersatz für den NitroSDK-SND-Treiber), siehe docs/sound.md */
#pragma once
#include <stdint.h>

/* Kanal 7 der IPC-FIFO: Adresse einer Befehlsliste (0 = nur "jetzt verarbeiten") */
void sound_command_list(uint32_t list);

/* Einmal pro VBlank: Zeit um ein DS-Bild weiterschalten (Sequenzen, Alarme, Status) */
void sound_vblank(void);

/* Mixer (32728 Hz, Stereo, int16): Zahl der seit dem letzten Aufruf erzeugten Samples
 * abholen. Liefert höchstens max Sample-Paare. */
int sound_take_samples(int16_t *out, int max);
