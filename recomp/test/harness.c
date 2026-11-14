/* Minimale Laufzeit für Tests.
 *
 * Bildet nur die Speicherbereiche nach, die Tests brauchen:
 *   0x01ff8000  ITCM      (32 KiB)
 *   0x02000000  Haupt-RAM (4 MiB)
 *   0x027e0000  DTCM      (16 KiB)
 * Alles andere liefert 0 und wird beim Schreiben ignoriert (mit Meldung).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"

static uint8_t itcm[0x8000];
static uint8_t ram[0x400000];
static uint8_t dtcm[0x4000];
unsigned harness_todo_count;

static uint8_t *ptr(uint32_t a) {
    if (a >= 0x01ff8000u && a < 0x02000000u) return &itcm[a - 0x01ff8000u];
    if (a >= 0x02000000u && a < 0x02400000u) return &ram[a - 0x02000000u];
    if (a >= 0x027e0000u && a < 0x027e4000u) return &dtcm[a - 0x027e0000u];
    return NULL;
}

static void unmapped(const char *op, uint32_t a) {
    static int count;
    if (count++ < 20) fprintf(stderr, "[harness] %s auf ungemappte Adresse 0x%08x\n", op, a);
}

uint8_t rd8(uint32_t a) { uint8_t *p = ptr(a); if (!p) { unmapped("rd8", a); return 0; } return *p; }
uint16_t rd16(uint32_t a) { a &= ~1u; return (uint16_t)(rd8(a) | rd8(a + 1) << 8); }
uint32_t rd32(uint32_t a) {
    uint32_t r = a & 3; a &= ~3u;
    uint32_t v = rd8(a) | rd8(a + 1) << 8 | rd8(a + 2) << 16 | (uint32_t)rd8(a + 3) << 24;
    return ROR32(v, r * 8);   /* ARM9: unausgerichtetes ldr rotiert */
}
void wr8(uint32_t a, uint32_t v) { uint8_t *p = ptr(a); if (!p) { unmapped("wr8", a); return; } *p = (uint8_t)v; }
void wr16(uint32_t a, uint32_t v) { a &= ~1u; wr8(a, v); wr8(a + 1, v >> 8); }
void wr32(uint32_t a, uint32_t v) { a &= ~3u; wr8(a, v); wr8(a + 1, v >> 8); wr8(a + 2, v >> 16); wr8(a + 3, v >> 24); }

int harness_load_arm9(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Kann %s nicht öffnen\n", path); return 0; }
    size_t n = fread(&ram[0x4000], 1, sizeof ram - 0x4000, f);
    fclose(f);
    return n > 0;
}

/* Indirekte Aufrufe/Sprünge: über die Funktionstabelle des Tests verteilen
 * (bei Einzeltests mit functable_empty.c gibt es keine Ziele -> wird gezählt) */
static int dispatch(CPU *c, uint32_t addr) {
    for (unsigned i = 0; i < rc_functable_count; i++)
        if (rc_functable[i].addr == (addr & ~1u)) { rc_functable[i].fn(c); return 1; }
    return 0;
}
void rc_call_indirect(CPU *c, uint32_t addr) {
    if (dispatch(c, addr)) return;
    fprintf(stderr, "[harness] call_indirect 0x%08x\n", addr); harness_todo_count++;
}
void rc_jump_indirect(CPU *c, uint32_t addr) {
    if (dispatch(c, addr)) return;
    fprintf(stderr, "[harness] jump_indirect 0x%08x\n", addr); harness_todo_count++;
}
void rc_fallthrough(CPU *c, uint32_t addr) { (void)c; fprintf(stderr, "[harness] fallthrough 0x%08x\n", addr); harness_todo_count++; }
void rc_swi(CPU *c, uint32_t n) { (void)c; fprintf(stderr, "[harness] swi 0x%x\n", n); harness_todo_count++; }
uint32_t rc_mrs(CPU *c, int spsr) { (void)spsr; return c->cpsr_ctrl; }
void rc_msr(CPU *c, const char *field, uint32_t v) { (void)field; c->cpsr_ctrl = v; }
uint32_t rc_mrc(CPU *c, int a, int b, int d, int e) { (void)c; (void)a; (void)b; (void)d; (void)e; return 0; }
void rc_mcr(CPU *c, int a, int b, int d, int e, uint32_t v) { (void)c; (void)a; (void)b; (void)d; (void)e; (void)v; }
void rc_todo(CPU *c, const char *what) { (void)c; fprintf(stderr, "[harness] TODO: %s\n", what); harness_todo_count++; }
