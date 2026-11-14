/* Test: übersetzte MI_CpuFillFast (ITCM 0x01ff8228).
 *
 * Diese Funktion löscht Speicher in 48-Byte-Blöcken und erledigt den Rest mit einem
 * berechneten Sprung mitten in eine Reihe von str-Befehlen ("sub pc, pc, r2").
 * Früher übersetzte der Recompiler das falsch und schrieb immer 48 Bytes zu viel.
 * Geprüft wird für alle Längen 0..0x200 (in 4er-Schritten):
 *   - genau [dst, dst+len) enthält den Füllwert
 *   - davor und dahinter bleibt alles unberührt (Schutzbytes)
 *   - Stackpointer und die gesicherten Register r4-r11 sind danach unverändert
 */
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "funcs.h"

#define BUF   0x02300000u
#define GUARD 0x100u

int main(void) {
    int fails = 0;
    for (uint32_t len = 0; len <= 0x200; len += 4) {
        for (uint32_t i = 0; i < len + 2 * GUARD; i++) wr8(BUF + i, 0xaa);
        uint32_t dst = BUF + GUARD, val = 0x11223344u ^ len;

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[0] = dst; c.r[1] = val; c.r[2] = len;
        for (int r = 4; r <= 11; r++) c.r[r] = 0x40000000u + (uint32_t)r;
        c.r[13] = HARNESS_STACK_TOP;
        c.r[14] = 0xdeadbeefu;
        rc_MI_CpuFillFast(&c);

        int ok = 1;
        for (uint32_t i = 0; i < GUARD; i++) ok &= rd8(BUF + i) == 0xaa;
        for (uint32_t i = 0; i < len; i += 4) ok &= rd32(dst + i) == val;
        for (uint32_t i = 0; i < GUARD; i++) ok &= rd8(dst + len + i) == 0xaa;
        if (!ok) { if (fails < 5) printf("Laenge 0x%x: falscher Speicherinhalt (Ueberlauf?)\n", len); fails++; }
        if (c.r[13] != HARNESS_STACK_TOP) { if (fails < 5) printf("Laenge 0x%x: Stackpointer falsch\n", len); fails++; }
        for (int r = 4; r <= 11; r++)
            if (c.r[r] != 0x40000000u + (uint32_t)r) { if (fails < 5) printf("Laenge 0x%x: r%d nicht erhalten\n", len, r); fails++; break; }
    }
    printf("MI_CpuFillFast: 129 Laengen, %d Fehler, %u TODOs\n", fails, harness_todo_count);
    printf(fails == 0 && harness_todo_count == 0 ? "OK\n" : "FEHLGESCHLAGEN\n");
    return fails != 0;
}
