/* Link-Test: bindet den gesamten erzeugten Code ein und gibt die Größe der
 * Funktionstabelle aus. Gestartet wird nichts – es geht nur darum, dass jeder
 * Aufruf ein Ziel hat. */
#include <stdio.h>
#include "harness.h"

int main(void) {
    printf("Funktionstabelle: %u Eintraege\n", rc_functable_count);
    return 0;
}
