/* Test: übersetzte Pad_Update (0x02019a74) gegen eine Referenz-Implementierung.
 *
 * Die Referenz ist nach dem Ghidra-Pseudocode von Hand geschrieben. Beide bekommen
 * dieselben zufälligen Tastenfolgen; nach jedem Aufruf müssen die Pad-Strukturen
 * Byte für Byte gleich sein. Die Masken-Tabelle (data_0204ac4c) kommt aus dem
 * echten ARM9-Programm.
 *
 * Bauen/Starten: siehe recomp/test/run_tests.bat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "funcs.h"

#define PAD_ADDR   0x02300000u   /* freier Platz im Haupt-RAM für die Pad-Struktur */
#define MASK_TABLE 0x0204ac4cu

/* Referenz nach Ghidra-Pseudocode */
static void ref_pad_update(uint8_t *p, uint16_t keys, const uint16_t *masks) {
    uint16_t held = (uint16_t)(p[0] | p[1] << 8);
    uint16_t trig = (uint16_t)(~held & keys);
    uint16_t rel = (uint16_t)(held & ~keys);
    uint16_t rep = 0;
    for (int i = 0; i < 10; i++) {
        uint16_t m = keys & masks[i];
        if (m == 0) {
            p[10 + i] = 0;
        } else if ((held & m) == 0) {
            rep |= m;
            p[10 + i] = p[8];
        } else {
            p[10 + i] = (uint8_t)(p[10 + i] - 1);
            if (p[10 + i] == 0) {
                rep |= m;
                p[10 + i] = p[9];
            }
        }
    }
    p[2] = trig & 0xff; p[3] = trig >> 8;
    p[4] = rel & 0xff;  p[5] = rel >> 8;
    p[6] = rep & 0xff;  p[7] = rep >> 8;
    p[0] = keys & 0xff; p[1] = keys >> 8;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: %s <arm9.bin>\n", argv[0]);
        return 2;
    }
    if (!harness_load_arm9(argv[1])) return 2;

    uint16_t masks[10];
    for (int i = 0; i < 10; i++) masks[i] = rd16(MASK_TABLE + 2 * i);
    printf("Masken-Tabelle:");
    for (int i = 0; i < 10; i++) printf(" %04x", masks[i]);
    printf("\n");

    uint8_t ref[22];
    memset(ref, 0, sizeof ref);
    ref[8] = 20;   /* Repeat-Startverzögerung */
    ref[9] = 4;    /* Repeat-Intervall */
    for (int i = 0; i < 22; i++) wr8(PAD_ADDR + i, ref[i]);

    srand(12345);
    int fails = 0;
    const int N = 100000;
    uint16_t keys = 0;
    for (int step = 0; step < N; step++) {
        /* meist Tasten halten, ab und zu wechseln -> Repeat-Logik wird geprüft */
        if (rand() % 8 == 0) keys = (uint16_t)(rand() & 0x0fff);

        CPU c;
        memset(&c, 0, sizeof c);
        c.r[0] = PAD_ADDR;
        c.r[1] = keys;
        c.r[13] = HARNESS_STACK_TOP;
        c.r[14] = 0xdeadbeefu;
        rc_Pad_Update(&c);
        ref_pad_update(ref, keys, masks);

        if (c.r[13] != HARNESS_STACK_TOP) {
            printf("Schritt %d: Stackpointer falsch (0x%08x)\n", step, c.r[13]);
            fails++;
        }
        for (int i = 0; i < 20; i++) {
            if (rd8(PAD_ADDR + i) != ref[i]) {
                if (fails < 10)
                    printf("Schritt %d: Byte %d ist 0x%02x, erwartet 0x%02x (Tasten %04x)\n",
                           step, i, rd8(PAD_ADDR + i), ref[i], keys);
                fails++;
                break;
            }
        }
    }
    printf("%d Schritte, %d Fehler, %u TODOs\n", N, fails, harness_todo_count);
    printf(fails == 0 && harness_todo_count == 0 ? "OK\n" : "FEHLGESCHLAGEN\n");
    return fails != 0;
}
