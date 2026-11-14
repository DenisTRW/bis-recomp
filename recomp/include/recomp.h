/* Laufzeit-Header für den vom Recompiler erzeugten Code.
 *
 * Der erzeugte Code arbeitet auf einem CPU-Kontext (Register + Flags) und greift
 * über rd8/rd16/rd32/wr8/wr16/wr32 auf den DS-Speicher zu. Diese Funktionen und
 * die rc_*-Hooks implementiert die Plattform-Schicht (bzw. ein Test-Harness).
 */
#pragma once
#include <stdint.h>

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v;   /* Flags, jeweils 0 oder 1 */
    uint32_t cpsr_ctrl;     /* Modus- und Interrupt-Bits (I, F, T, Modus) */
} CPU;

typedef void (*RcFunc)(CPU *c);

typedef struct RcFuncEntry {
    uint32_t addr;
    int overlay;            /* -1 = Hauptprogramm/ITCM/DTCM */
    RcFunc fn;
    const char *name;
} RcFuncEntry;

extern const RcFuncEntry rc_functable[];
extern const unsigned rc_functable_count;

/* ---- Speicher (Plattform-Schicht) */
uint8_t  rd8(uint32_t a);
uint16_t rd16(uint32_t a);
uint32_t rd32(uint32_t a);
void wr8(uint32_t a, uint32_t v);
void wr16(uint32_t a, uint32_t v);
void wr32(uint32_t a, uint32_t v);

/* ---- Hooks (Plattform-Schicht) */
void rc_call_indirect(CPU *c, uint32_t addr);   /* Aufruf über Zeiger */
void rc_jump_indirect(CPU *c, uint32_t addr);   /* Sprung über Zeiger (Tail-Call) */
void rc_fallthrough(CPU *c, uint32_t addr);     /* Funktion endet ohne Rücksprung */
void rc_swi(CPU *c, uint32_t n);                /* BIOS-Aufruf */
uint32_t rc_mrs(CPU *c, int spsr);
void rc_msr(CPU *c, const char *field, uint32_t v);
uint32_t rc_mrc(CPU *c, int op1, int crn, int crm, int op2);
void rc_mcr(CPU *c, int op1, int crn, int crm, int op2, uint32_t v);
void rc_todo(CPU *c, const char *what);         /* nicht übersetzter Befehl */

#define RC_TODO(s) rc_todo(c, s)

/* Optionaler Trace-Haken am Anfang jeder Funktion (mit -DRC_TRACE bauen) */
#ifdef RC_TRACE
void rc_enter(CPU *c, uint32_t addr);
#define RC_ENTER(a) rc_enter(c, (a))
#else
#define RC_ENTER(a) ((void)0)
#endif

/* ---- Flags */
#define NZ(x) do { uint32_t nz_ = (x); c->n = nz_ >> 31; c->z = (nz_ == 0); } while (0)

static inline uint32_t rc_adc(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)r;
    c->n = res >> 31;
    c->z = res == 0;
    c->cf = (uint32_t)(r >> 32);
    c->v = ((~(a ^ b) & (a ^ res)) >> 31) & 1;
    return res;
}
/* a - b - !cin  (ARM: Carry = kein Borrow) */
static inline uint32_t rc_sbc(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    return rc_adc(c, a, ~b, cin);
}
#define ADC_FLAGS(a, b, cin) rc_adc(c, (a), (b), (cin))
#define SBC_FLAGS(a, b, cin) rc_sbc(c, (a), (b), (cin))
#define ADD_FLAGS(a, b) ((void)rc_adc(c, (a), (b), 0))
#define SUB_FLAGS(a, b) ((void)rc_sbc(c, (a), (b), 1))

/* ---- Verschiebungen mit ARM-Semantik */
static inline uint32_t ROR32(uint32_t x, uint32_t n) {
    n &= 31;
    return n ? (x >> n) | (x << (32 - n)) : x;
}
static inline uint32_t LSL_R(uint32_t x, uint32_t s) { s &= 0xff; return s >= 32 ? 0 : x << s; }
static inline uint32_t LSR_R(uint32_t x, uint32_t s) { s &= 0xff; return s >= 32 ? 0 : x >> s; }
static inline uint32_t ASR_R(uint32_t x, uint32_t s) {
    s &= 0xff;
    return (uint32_t)((int32_t)x >> (s >= 32 ? 31 : s));
}
static inline uint32_t ROR_R(uint32_t x, uint32_t s) { return ROR32(x, s & 0xff); }

static inline uint32_t LSL_R_C(uint32_t x, uint32_t s, uint32_t c) {
    s &= 0xff;
    if (s == 0) return c;
    if (s < 32) return (x >> (32 - s)) & 1;
    if (s == 32) return x & 1;
    return 0;
}
static inline uint32_t LSR_R_C(uint32_t x, uint32_t s, uint32_t c) {
    s &= 0xff;
    if (s == 0) return c;
    if (s <= 32) return (x >> (s - 1)) & 1;
    return 0;
}
static inline uint32_t ASR_R_C(uint32_t x, uint32_t s, uint32_t c) {
    s &= 0xff;
    if (s == 0) return c;
    if (s < 32) return (x >> (s - 1)) & 1;
    return x >> 31;
}
static inline uint32_t ROR_R_C(uint32_t x, uint32_t s, uint32_t c) {
    s &= 0xff;
    if (s == 0) return c;
    s &= 31;
    return s ? (x >> (s - 1)) & 1 : x >> 31;
}

static inline uint32_t CLZ32(uint32_t x) {
    uint32_t n = 0;
    if (x == 0) return 32;
    while (!(x & 0x80000000u)) { x <<= 1; n++; }
    return n;
}
