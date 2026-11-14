/* Fiber-Scheduler – siehe sched.h */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "sched.h"

#define MAX_THREADS   64
#define STACK_COMMIT  (256 * 1024)
#define STACK_RESERVE (32 * 1024 * 1024)   /* recompilierter Code kann tief verschachteln */
#define CANARY_WORDS  8

enum { SLOT_FREE = 0, SLOT_ALIVE, SLOT_FINISHED };

typedef struct {
    uint32_t key;
    int state;
    LPVOID fiber;
    SchedEntry entry;
    void *arg;
    CPU saved;                 /* CPU-Kontext, während der Thread schläft */
    uint64_t saved_sum;
    volatile uint64_t *canary; /* liegt am Boden des eigenen Fiber-Stacks */
    int resumer;               /* wer zuletzt zu diesem Thread gewechselt hat */
} Slot;

static Slot slots[MAX_THREADS];
static int cur = -1;           /* -1 = Wurzel-Fiber (kein Spiel-Thread) */
static LPVOID root_fiber;
static CPU *gc;
unsigned sched_violations;

/* ---------------------------------------------------------------- Trace */

#define TRACE_RING 4096
static struct { int type; uint32_t a, b; } ring[TRACE_RING];
static uint64_t tr_count, tr_hash;

static void hash_u32(uint32_t v) {
    for (int i = 0; i < 4; i++) { tr_hash ^= (v >> (8 * i)) & 0xff; tr_hash *= 0x100000001b3ull; }
}

void sched_trace(int type, uint32_t a, uint32_t b) {
    ring[tr_count % TRACE_RING].type = type;
    ring[tr_count % TRACE_RING].a = a;
    ring[tr_count % TRACE_RING].b = b;
    tr_count++;
    hash_u32((uint32_t)type); hash_u32(a); hash_u32(b);
}
uint64_t sched_trace_hash(void) { return tr_hash; }
uint64_t sched_trace_count(void) { return tr_count; }

void sched_trace_dump(FILE *f, unsigned last_n) {
    static const char *const names[] = { "?", "CREATE", "SWITCH", "EXIT", "IRQ", "TICK", "WAKE", "USER" };
    uint64_t start = tr_count > last_n ? tr_count - last_n : 0;
    if (tr_count - start > TRACE_RING) start = tr_count - TRACE_RING;
    for (uint64_t i = start; i < tr_count; i++) {
        int t = ring[i % TRACE_RING].type;
        fprintf(f, "  %8llu  %-6s %08x %08x\n", (unsigned long long)i,
                (t > 0 && t <= TR_USER) ? names[t] : "?", ring[i % TRACE_RING].a, ring[i % TRACE_RING].b);
    }
}

/* ---------------------------------------------------------------- Prüfungen */

static uint64_t cpu_sum(const CPU *c) {
    const uint8_t *p = (const uint8_t *)c;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof *c; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static uint64_t canary_value(uint32_t key, int i) {
    return 0x5ca1ab1e00000000ull ^ ((uint64_t)key << 8) ^ (uint64_t)(i * 0x9e3779b9u);
}

static void violation(const char *what, uint32_t key) {
    sched_violations++;
    if (sched_violations <= 20) fprintf(stderr, "[sched] VERSTOSS: %s (Thread %08x)\n", what, key);
}

static void check_slot(const Slot *s) {
    if (s->canary) {
        for (int i = 0; i < CANARY_WORDS; i++)
            if (s->canary[i] != canary_value(s->key, i)) { violation("C-Stack-Kanarienvogel beschaedigt", s->key); break; }
    }
    if (cpu_sum(&s->saved) != s->saved_sum) violation("gesicherter CPU-Kontext beschaedigt", s->key);
}

void sched_check_all(void) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (slots[i].state == SLOT_ALIVE && i != cur && slots[i].fiber) check_slot(&slots[i]);
}

/* ---------------------------------------------------------------- Verwaltung */

static int find(uint32_t key) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (slots[i].state != SLOT_FREE && slots[i].key == key) return i;
    return -1;
}

uint32_t sched_current(void) { return cur >= 0 ? slots[cur].key : 0; }
int sched_exists(uint32_t key) { int i = find(key); return i >= 0 && slots[i].state == SLOT_ALIVE; }
int sched_finished(uint32_t key) { int i = find(key); return i >= 0 && slots[i].state == SLOT_FINISHED; }
unsigned sched_thread_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_THREADS; i++) n += slots[i].state == SLOT_ALIVE;
    return n;
}

static void save_current(void) {
    if (cur < 0) return;
    slots[cur].saved = *gc;
    slots[cur].saved_sum = cpu_sum(&slots[cur].saved);
}

/* Wird im eigenen Fiber nach dem Fortsetzen aufgerufen */
static void resume_self(int me) {
    cur = me;
    Slot *s = &slots[me];
    check_slot(s);
    *gc = s->saved;
}

static void jump_to(int target) {
    if (target < 0) { cur = -1; SwitchToFiber(root_fiber); return; }
    cur = target;
    SwitchToFiber(slots[target].fiber);
}

static void WINAPI fiber_proc(LPVOID p) {
    Slot *s = (Slot *)p;
    int me = (int)(s - slots);
    volatile uint64_t canary[CANARY_WORDS];
    for (int i = 0; i < CANARY_WORDS; i++) canary[i] = canary_value(s->key, i);
    s->canary = canary;
    cur = me;
    memset(gc, 0, sizeof *gc);   /* neuer Thread startet mit definiertem Zustand */
    s->entry(gc, s->arg);

    /* Thread-Funktion ist zurückgekehrt */
    s->state = SLOT_FINISHED;
    s->canary = NULL;
    int back = s->resumer;
    if (back >= 0 && slots[back].state != SLOT_ALIVE) back = -1;
    sched_trace(TR_EXIT, s->key, back >= 0 ? slots[back].key : 0);
    for (;;) {                   /* ein Fiber darf nie zurückkehren */
        if (back >= 0) jump_to(back); else jump_to(-1);
        violation("beendeter Thread wurde fortgesetzt", s->key);
        back = -1;
    }
}

static int create(uint32_t key, SchedEntry entry, void *arg) {
    int i = find(key);
    if (i >= 0 && slots[i].state == SLOT_FINISHED) {
        /* Schlüssel wird wiederverwendet (z. B. neuer Thread an derselben Adresse) */
        if (slots[i].fiber) DeleteFiber(slots[i].fiber);
        memset(&slots[i], 0, sizeof slots[i]);
    }
    for (i = 0; i < MAX_THREADS && slots[i].state != SLOT_FREE; i++) {}
    if (i == MAX_THREADS) { violation("zu viele Threads", key); return -1; }
    Slot *s = &slots[i];
    memset(s, 0, sizeof *s);
    s->key = key;
    s->state = SLOT_ALIVE;
    s->entry = entry;
    s->arg = arg;
    s->fiber = CreateFiberEx(STACK_COMMIT, STACK_RESERVE, FIBER_FLAG_FLOAT_SWITCH, fiber_proc, s);
    if (!s->fiber) { violation("CreateFiberEx fehlgeschlagen", key); s->state = SLOT_FREE; return -1; }
    sched_trace(TR_CREATE, key, 0);
    return i;
}

static int irq_depth;

void sched_switch(CPU *c, uint32_t key, SchedEntry entry, void *arg) {
    gc = c;
    if (irq_depth) {
        /* Entwurfsregel: nie innerhalb eines Interrupts umschalten, der Wechsel
         * passiert erst danach am sicheren Punkt. */
        violation("Thread-Wechsel innerhalb eines Interrupts", key);
        return;
    }
    if (cur >= 0 && slots[cur].key == key) return;      /* schon dran */
    int t = find(key);
    if (t >= 0 && slots[t].state == SLOT_FINISHED) {
        if (!entry) { violation("Wechsel zu beendetem Thread", key); return; }
        t = -1;
    }
    if (t < 0) {
        if (!entry) { violation("Wechsel zu unbekanntem Thread ohne Startfunktion", key); return; }
        t = create(key, entry, arg);
        if (t < 0) return;
    }
    int me = cur;
    save_current();
    slots[t].resumer = me;
    sched_trace(TR_SWITCH, me >= 0 ? slots[me].key : 0, key);
    jump_to(t);
    /* ---- hier geht es weiter, wenn jemand zu uns zurückwechselt ---- */
    resume_self(me);
}

void sched_irq(CPU *c, uint32_t bits, SchedEntry handler, void *arg) {
    CPU saved = *c;
    int me = cur;
    sched_trace(TR_IRQ, bits, sched_current());
    irq_depth++;
    handler(c, arg);
    irq_depth--;
    if (cur != me) violation("Interrupt endet in einem anderen Thread", sched_current());
    *c = saved;
}

static int stopping;
void sched_stop(CPU *c) {
    gc = c;
    stopping = 1;
    save_current();
    jump_to(-1);
}

void sched_run(CPU *c, uint32_t main_key, SchedEntry entry, void *arg) {
    gc = c;
    if (!IsThreadAFiber()) root_fiber = ConvertThreadToFiber(NULL);
    else root_fiber = GetCurrentFiber();
    for (int i = 0; i < MAX_THREADS; i++)
        if (slots[i].fiber) DeleteFiber(slots[i].fiber);
    memset(slots, 0, sizeof slots);
    cur = -1;
    stopping = 0;
    tr_count = 0;
    tr_hash = 0xcbf29ce484222325ull;

    int m = create(main_key, entry, arg);
    if (m < 0) return;
    slots[m].resumer = -1;
    jump_to(m);
    /* zurück im Wurzel-Fiber: Haupt-Thread fertig oder sched_stop() */
    cur = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (slots[i].fiber) DeleteFiber(slots[i].fiber);
        slots[i].fiber = NULL;
        if (slots[i].state == SLOT_ALIVE) slots[i].state = SLOT_FINISHED;
    }
}

/* ---------------------------------------------------------------- Test-Hilfen */

void sched_debug_corrupt_slot(uint32_t key) {
    int i = find(key);
    if (i >= 0) slots[i].saved.r[7] ^= 0x1234;
}
void sched_debug_corrupt_canary(uint32_t key) {
    int i = find(key);
    if (i >= 0 && slots[i].canary) ((uint64_t *)slots[i].canary)[3] ^= 1;
}
