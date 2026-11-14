/* HLE-Ersatz für den Thread-Wechsel des NitroSDK (siehe docs/threads.md).
 *
 * Grundregel: WER läuft, entscheidet weiterhin der Original-Code
 * (OS_SelectThread / OS_RescheduleThread). Ersetzt wird nur das WIE des
 * Umschaltens: statt Register in den DS-Speicher zu schreiben und per
 * "subs pc, lr, #4" in einen anderen Thread zu springen, wechselt der
 * Scheduler zwischen Host-Fibers.
 */
#include <stdint.h>
#include <stdio.h>
#include "platform.h"
#include "sched.h"
#include "funcs.h"

/* Adressen aus der Analyse (EU, CLJP Rev 0) */
#define IRQ_THREAD_QUEUE 0x027e11f0u   /* Threads, die auf einen Interrupt warten */
#define RESCHED_FLAG     0x02060604u   /* u16: Thread-Wechsel angefordert */
#define IDLE_LIMIT       600           /* so viele Leerlauf-Frames am Stück = Deadlock */

/* ---- Kontext-Layout des SDK (Anfang der Thread-Struktur) */
#define CTX_CPSR 0x00
#define CTX_R0   0x04   /* r0..r12 */
#define CTX_SP   0x38
#define CTX_LR   0x3c
#define CTX_PC4  0x40   /* pc + 4 */
#define CTX_CP   0x48   /* Divider-Kontext */

int platform_resched_pending;

static uint32_t cpsr_of(const CPU *c) {
    return (c->cpsr_ctrl & 0x0fffffffu) | c->n << 31 | c->z << 30 | c->cf << 29 | c->v << 28;
}

/* Ruft eine recompilierte Funktion mit r0 = arg auf, ohne den restlichen Kontext zu verändern */
static void call_preserving(CPU *c, void (*fn)(CPU *), uint32_t arg) {
    CPU keep = *c;
    c->r[0] = arg;
    fn(c);
    *c = keep;
}

/* ---------------------------------------------------------------- OS_SaveContext */

void hle_OS_SaveContext(CPU *c) {
    uint32_t ctx = c->r[0];
    HLE(ORIGINAL, "OS_SaveContext: Kontext sichern, Fiber an Thread binden");
    call_preserving(c, rc_CP_SaveContext, ctx + CTX_CP);

    /* Kontext auch in den DS-Speicher schreiben, damit Spielcode/Debugger ihn sehen */
    wr32(ctx + CTX_CPSR, cpsr_of(c));
    wr32(ctx + CTX_R0, 1);                      /* wie Original: r0 = 1 beim Fortsetzen */
    for (int i = 1; i <= 12; i++) wr32(ctx + CTX_R0 + 4 * (uint32_t)i, c->r[i]);
    wr32(ctx + CTX_SP, c->r[13]);
    wr32(ctx + CTX_LR, c->r[14]);
    wr32(ctx + CTX_PC4, 0);                     /* Fortsetzung läuft über den Fiber */

    uint32_t me = sched_current();
    if (me == BOOT_THREAD_KEY) sched_rebind_current(ctx);
    else if (me != ctx) platform_stop("OS_SaveContext fuer einen fremden Thread");

    c->r[0] = 0;                                /* "gerade gesichert" */
}

/* ---------------------------------------------------------------- OS_LoadContext */

/* Start eines Threads, der noch nie lief (angelegt mit OS_CreateThread) */
static void ds_thread_start(CPU *c, void *arg) {
    uint32_t ctx = (uint32_t)(uintptr_t)arg;
    uint32_t cpsr = rd32(ctx + CTX_CPSR);
    c->cpsr_ctrl = cpsr & 0x0fffffffu;
    c->n = cpsr >> 31; c->z = (cpsr >> 30) & 1; c->cf = (cpsr >> 29) & 1; c->v = (cpsr >> 28) & 1;
    for (int i = 0; i <= 12; i++) c->r[i] = rd32(ctx + CTX_R0 + 4 * (uint32_t)i);
    c->r[13] = rd32(ctx + CTX_SP);
    c->r[14] = rd32(ctx + CTX_LR);
    uint32_t pc = rd32(ctx + CTX_PC4) - 4;
    uint32_t ret = c->r[14];
    c->r[15] = pc;

    fprintf(stderr, "  [thread] Start %08x: pc=%08x sp=%08x r0=%08x lr=%08x\n", ctx, pc, c->r[13], c->r[0], ret);
    if (!platform_is_function(pc)) platform_stop("Thread-Start nicht an einem Funktionsanfang");
    HLE(ORIGINAL, "Thread-Start aus dem SDK-Kontext (pc, sp, r0, lr)");
    rc_call_indirect(c, pc);
    /* Thread-Funktion ist zurückgekehrt: weiter bei lr (normalerweise OS_ExitThread) */
    rc_call_indirect(c, ret);
    platform_stop("Thread lief nach seinem Ende weiter");
}

void hle_OS_LoadContext(CPU *c) {
    uint32_t ctx = c->r[0];
    HLE(ORIGINAL, "OS_LoadContext: Wechsel zum Fiber des Ziel-Threads");
    call_preserving(c, rc_CP_RestoreContext, ctx + CTX_CP);
    if (ctx == sched_current()) platform_stop("OS_LoadContext auf den eigenen Thread");
    sched_switch(c, ctx, ds_thread_start, (void *)(uintptr_t)ctx);
    /* Wir laufen wieder: entspricht im Original "OS_SaveContext gab 1 zurück".
     * Der Aufrufer (OS_RescheduleThread) kehrt danach einfach zurück. */
}

/* ---------------------------------------------------------------- OS_SelectThread */

void hle_OS_SelectThread(CPU *c) {
    for (int idle = 0;; idle++) {
        rc_orig_OS_SelectThread(c);
        if (c->r[0] != 0) {
            platform_resched_pending = 0;
            return;
        }
        if (idle >= IDLE_LIMIT) platform_stop("Deadlock: kein Thread wird mehr bereit");
        HLE(SIMULIERT, "Leerlauf: kein Thread bereit -> Zeit bis zum naechsten Interrupt");
        platform_idle(c);
    }
}

/* ---------------------------------------------------------------- IRQ-Thread-Wechsel */

void hle_OS_IrqHandler_ThreadSwitch(CPU *c) {
    /* Teil 1 wie im Original: alle Threads der IRQ-Warteschlange wecken */
    uint32_t t = rd32(IRQ_THREAD_QUEUE);
    if (t) {
        HLE(ORIGINAL, "IRQ: wartende Threads wecken");
        while (t) {
            uint32_t next = rd32(t + 0x80);
            wr32(t + 0x64, 1);   /* bereit */
            wr32(t + 0x78, 0);
            wr32(t + 0x7c, 0);
            wr32(t + 0x80, 0);
            sched_trace(TR_WAKE, t, 0);
            t = next;
        }
        wr32(IRQ_THREAD_QUEUE, 0);
        wr32(IRQ_THREAD_QUEUE + 4, 0);
        wr16(RESCHED_FLAG, 1);
    }
    /* Teil 2: Wechsel nicht im Interrupt, sondern danach am sicheren Punkt */
    if (rd16(RESCHED_FLAG)) {
        wr16(RESCHED_FLAG, 0);
        platform_resched_pending = 1;
        HLE(SIMULIERT, "IRQ: Thread-Wechsel erst nach dem Interrupt am sicheren Punkt");
    }
    c->r[13] += 4;   /* wie "ldr pc, [sp], #4": von OS_IrqHandler gesichertes lr abräumen */
}
