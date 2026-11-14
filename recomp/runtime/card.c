/* Spielkarte (ROM-Bus) aus Sicht des ARM9.
 *
 * Das NitroSDK liest ROM-Daten direkt über die Karten-Register:
 *   0x040001a4  ROMCTRL   Bit 31 Start/Busy, Bit 23 Datenwort bereit,
 *                         Bits 24-26 Blockgröße (0=0, 1..6 = 0x100<<n, 7=4 Bytes)
 *   0x040001a8  8 Befehls-Bytes (z. B. B7 aa aa aa aa 00 00 00 = Daten lesen ab aa)
 *   0x04100010  Datenport (32 Bit pro Lesezugriff)
 * Die KEY1/KEY2-Verschlüsselung auf dem Bus macht die Hardware selbst; für die
 * Software ist sie unsichtbar und wird hier nicht nachgebildet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "card.h"

static uint8_t *rom;
static uint32_t rom_size;

#define CHIP_ID 0x00007fc2u   /* Hersteller 0xC2, 128 MB (0x7F) */

uint32_t card_chip_id(void) { return CHIP_ID; }
const uint8_t *card_rom(void) { return rom; }
uint32_t card_rom_size(void) { return rom_size; }

int card_load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Kann ROM %s nicht oeffnen\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc((size_t)n);
    rom_size = rom && fread(rom, 1, (size_t)n, f) == (size_t)n ? (uint32_t)n : 0;
    fclose(f);
    return rom_size != 0;
}

/* ---------------------------------------------------------------- Übertragung */

static uint8_t cmd[8];
static uint32_t xfer_addr, xfer_left, xfer_kind;   /* kind: 0 = Daten, 1 = Chip-ID, 2 = Dummy */
static uint32_t romctrl;

void card_write_cmd(uint32_t off, uint32_t v, int size) {
    for (int i = 0; i < size && off + (uint32_t)i < 8; i++) cmd[off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static uint32_t block_bytes(uint32_t ctrl) {
    uint32_t bs = (ctrl >> 24) & 7;
    if (bs == 0) return 0;
    if (bs == 7) return 4;
    return 0x100u << bs;
}

/* Schreiben auf ROMCTRL; Rückgabe: neuer Registerwert */
uint32_t card_write_romctrl(uint32_t v) {
    romctrl = v & ~0x00800000u;
    if (!(v & 0x80000000u)) return romctrl;
    xfer_left = block_bytes(v);
    switch (cmd[0]) {
    case 0xb7:
        xfer_kind = 0;
        xfer_addr = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
        HLE(ORIGINAL, "Karte: Befehl B7 (ROM-Daten lesen)");
        if (getenv("BIS_CARDLOG")) fprintf(stderr, "  [karte] B7 %08x, %u Bytes\n", xfer_addr, block_bytes(v));
        break;
    case 0xb8:
        xfer_kind = 1;
        HLE(ORIGINAL, "Karte: Befehl B8 (Chip-ID)");
        break;
    default:
        xfer_kind = 2;
        HLE(FEHLT, "Karte: unbekannter Befehl (liefert 0xFFFFFFFF)");
        break;
    }
    if (xfer_left) romctrl |= 0x00800000u;    /* erstes Wort bereit */
    else romctrl &= ~0x80000000u;             /* nichts zu übertragen: sofort fertig */
    return romctrl;
}

uint32_t card_read_romctrl(void) { return romctrl; }

int card_transfer_active(void) { return (romctrl & 0x80000000u) && xfer_left; }

uint32_t card_read_data(void) {
    if (!xfer_left) return 0xffffffffu;
    uint32_t v = 0xffffffffu;
    if (xfer_kind == 0) {
        /* Bereich unter 0x8000 (Secure Area) liefert auf echten Karten eine
         * Wiederholung ab 0x8000 – kommt im normalen Spielbetrieb nicht vor */
        uint32_t a = xfer_addr < 0x8000 ? 0x8000 + (xfer_addr & 0x1ff) : xfer_addr;
        if (a + 4 <= rom_size) v = rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | (uint32_t)rom[a + 3] << 24;
        xfer_addr += 4;
    } else if (xfer_kind == 1) {
        v = CHIP_ID;
    }
    xfer_left -= 4;
    if (!xfer_left) romctrl &= ~0x80800000u;  /* fertig: Busy und Datenwort-Bit aus */
    return v;
}
