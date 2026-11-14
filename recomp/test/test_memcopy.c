/* Test: übersetzte MI_CpuCopyFast (ITCM 0x01ff82ac) samt Rest-Blöcken.
 *
 * Kopiert in 44-Byte-Blöcken (ldm/stm mit 11 Registern) und springt für den Rest
 * per "sub pc, pc, r2, lsl #2" in eine Tabelle aus 16-Byte-Blöcken HINTER der
 * Funktion (0x01ff82e0 .. 0x01ff8380). Jeder Block kopiert 10..0 Wörter und kehrt
 * mit dem von MI_CpuCopyFast gesicherten lr zurück. Der Test braucht deshalb
 * echte indirekte Sprünge zwischen Funktionen (Funktionstabelle).
 * Geprüft: alle Längen 0..0x200, genaues Ziel, Schutzbytes, sp und r4-r11.
 */
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "funcs.h"

#define SRC   0x02300000u
#define DST   0x02310000u
#define GUARD 0x100u

int main(void) {
    int fails = 0;
    for (uint32_t i = 0; i < 0x400; i += 4) wr32(SRC + i, 0xc0de0000u + i);
    for (uint32_t len = 0; len <= 0x200; len += 4) {
        for (uint32_t i = 0; i < len + 2 * GUARD; i++) wr8(DST + i, 0xaa);
        uint32_t dst = DST + GUARD;

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[0] = SRC; c.r[1] = dst; c.r[2] = len;
        for (int r = 4; r <= 11; r++) c.r[r] = 0x40000000u + (uint32_t)r;
        c.r[13] = HARNESS_STACK_TOP;
        c.r[14] = 0xdeadbeefu;
        rc_MI_CpuCopyFast(&c);

        int ok = 1;
        for (uint32_t i = 0; i < GUARD; i++) ok &= rd8(DST + i) == 0xaa;
        for (uint32_t i = 0; i < len; i += 4) ok &= rd32(dst + i) == 0xc0de0000u + i;
        for (uint32_t i = 0; i < GUARD; i++) ok &= rd8(dst + len + i) == 0xaa;
        if (!ok) { if (fails < 5) printf("Laenge 0x%x: falscher Speicherinhalt\n", len); fails++; }
        if (c.r[13] != HARNESS_STACK_TOP) { if (fails < 5) printf("Laenge 0x%x: Stackpointer falsch (%08x)\n", len, c.r[13]); fails++; }
        for (int r = 4; r <= 11; r++)
            if (c.r[r] != 0x40000000u + (uint32_t)r) { if (fails < 5) printf("Laenge 0x%x: r%d nicht erhalten\n", len, r); fails++; break; }
    }
    printf("MI_CpuCopyFast: 129 Laengen, %d Fehler, %u TODOs\n", fails, harness_todo_count);

    /* MI_CpuCopyFastBackward (0x01ff8380): kopiert vom Ende her (für überlappende
     * Bereiche mit Ziel > Quelle). Gleiche Rest-Block-Technik (0x01ff83bc ...). */
    int fails_b = 0;
    for (uint32_t len = 0; len <= 0x200; len += 4) {
        /* überlappend: Ziel liegt 8 Bytes hinter der Quelle */
        uint32_t src = DST + GUARD, dst = src + 8;
        for (uint32_t i = 0; i < GUARD; i++) wr8(DST + i, 0xaa);
        for (uint32_t i = 0; i < len + 8 + GUARD; i += 4) wr32(src + i, 0xbeef0000u + i);
        CPU c;
        memset(&c, 0, sizeof c);
        c.r[0] = src; c.r[1] = dst; c.r[2] = len;
        for (int r = 4; r <= 11; r++) c.r[r] = 0x40000000u + (uint32_t)r;
        c.r[13] = HARNESS_STACK_TOP;
        rc_MI_CpuCopyFastBackward(&c);
        int ok = 1;
        for (uint32_t i = 0; i < GUARD; i++) ok &= rd8(DST + i) == 0xaa;
        for (uint32_t i = 0; i < 8; i += 4) ok &= rd32(src + i) == 0xbeef0000u + i;          /* Quelle vorn unberührt */
        for (uint32_t i = 0; i < len; i += 4) ok &= rd32(dst + i) == 0xbeef0000u + i;        /* wie memmove */
        for (uint32_t i = len + 8; i < len + 8 + GUARD; i += 4) ok &= rd32(src + i) == 0xbeef0000u + i;
        if (!ok) { if (fails_b < 5) printf("Rueckwaerts, Laenge 0x%x: falscher Speicherinhalt\n", len); fails_b++; }
        if (c.r[13] != HARNESS_STACK_TOP) { if (fails_b < 5) printf("Rueckwaerts, Laenge 0x%x: Stackpointer falsch\n", len); fails_b++; }
        for (int r = 4; r <= 11; r++)
            if (c.r[r] != 0x40000000u + (uint32_t)r) { if (fails_b < 5) printf("Rueckwaerts, Laenge 0x%x: r%d nicht erhalten\n", len, r); fails_b++; break; }
    }
    printf("MI_CpuCopyFastBackward (ueberlappend): 129 Laengen, %d Fehler, %u TODOs\n", fails_b, harness_todo_count);
    fails += fails_b;
    printf(fails == 0 && harness_todo_count == 0 ? "OK\n" : "FEHLGESCHLAGEN\n");
    return fails != 0;
}
