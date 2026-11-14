/* Selbstmodifizierender Code.
 *
 * Statisch übersetzter Code kennt nur die Befehle aus dem ROM. Ändert das Spiel zur
 * Laufzeit Code im Speicher, muss die übersetzte Funktion nachsehen, was gerade dort
 * steht, und genau diese Befehle ausführen. Unbekannter Inhalt hält den Lauf an,
 * statt still das Falsche zu tun.
 *
 * Bekannte Stellen:
 *
 * 0x01ff8d18 (ITCM), Kopierschutz DSProtect (ov138/ov139):
 *   Beim Start entschlüsselt ov139 fünf Worte und schreibt sie über den Anfang der
 *   Frame-Funktion func_01ff8d18 – aber nur, wenn die Prüfungen in ov138 bestehen
 *   (u. a. Karten-Lesezugriffe unter 0x8000, MAC-Adresse ungleich 0). Die Hauptschleife
 *   (func_01ff8bb8) schreibt vor ihrem Aufruf jedes Frame das Original zurück und
 *   setzt danach den Patch wieder ein. Zwischendurch springt der verschleierte
 *   Dispatcher der Szenen-Overlays (z. B. ov129 0x0206db20) hierher; mit Patch führt
 *   das zu Pad_ReadAndUpdate. Ohne Patch liest die Dateiauswahl keine Tasten.
 *
 *   Patch:  ldr r0, [r0, #8]          ; zweiter verschlüsselter Zeiger der Tabelle
 *           eor r0, r1, r0, ror r1
 *           ldr r1, [pc]              ; = Wort 5 (zweiter Schlüssel)
 *           eor pc, r1, r0, ror r1    ; Sprung, lr bleibt
 *           .word <Schlüssel>
 */
#include <stdio.h>
#include "platform.h"
#include "funcs.h"

static inline uint32_t ror32(uint32_t x, uint32_t rs) {   /* ROR um Register: Betrag = Rs & 0xff */
    uint32_t n = rs & 31;
    return n ? (x >> n) | (x << (32 - n)) : x;
}

void hle_itcm_01ff8d18(CPU *c) {
    platform_inhibit_irq++;
    uint32_t w[5];
    for (int i = 0; i < 5; i++) w[i] = rd32(0x01ff8d18u + 4u * (uint32_t)i);
    if (w[0] == 0xe92d4010u && w[1] == 0xe59f1044u && w[2] == 0xe59f0044u && w[3] == 0xe5912000u && w[4] == 0xe5901000u) {
        platform_inhibit_irq--;
        HLE(ORIGINAL, "ITCM 0x01ff8d18: Originalcode (Frame-Funktion)");
        rc_orig_func_01ff8d18(c);
        return;
    }
    if (w[0] == 0xe5900008u && w[1] == 0xe0210170u && w[2] == 0xe59f1000u && w[3] == 0xe021f170u) {
        uint32_t r0 = rd32(c->r[0] + 8);
        platform_inhibit_irq--;
        r0 = c->r[1] ^ ror32(r0, c->r[1]);
        uint32_t r1 = w[4];
        c->r[0] = r0;
        c->r[1] = r1;
        HLE(ORIGINAL, "ITCM 0x01ff8d18: Kopierschutz-Patch ausgefuehrt (selbstmodifizierender Code)");
        rc_call_indirect(c, r1 ^ ror32(r0, r1));
        return;
    }
    platform_inhibit_irq--;
    fprintf(stderr, "  [smc] unbekannter Code bei 0x01ff8d18: %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4]);
    platform_stop("Selbstmodifizierender Code mit unbekanntem Inhalt (0x01ff8d18)");
}
