/* Start-Test: lässt den recompilierten Spielcode ab dem Start laufen und
 * berichtet, wie weit er kommt.
 *
 * Das ist die Keimzelle der Plattform-Schicht:
 *   - Speicherkarte: ITCM, Haupt-RAM (+Spiegel), Shared WRAM, DTCM, I/O,
 *     Paletten, VRAM, OAM
 *   - I/O: VCOUNT/DISPSTAT, Timer, Hardware-Divider/Sqrt, DMA (sofort), IF/IE/IME
 *   - BIOS: die SWIs, die das Spiel benutzt
 *   - VBlank-Interrupt: ruft den Interrupt-Handler des Spiels auf
 *   - Funktionstabelle für indirekte Aufrufe, Overlays werden anhand des
 *     RAM-Inhalts erkannt
 *   - Wachhund: bricht nach einem Speicherzugriffs-Budget oder N Frames ab
 *
 * Aufruf: boot_test.exe <extract/eur-Ordner> [frames] [budget_mio]
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"
#include "funcs.h"

/* ================================================================ HLE-Register
 *
 * Jeder Hardware-Ersatz wird einer von drei Kategorien zugeordnet und gezählt:
 *   ORIGINAL   – verhält sich wie die echte Hardware (z. B. Hardware-Divider)
 *   SIMULIERT  – bewusst vereinfachte Antwort, damit der Code weiterläuft
 *                (z. B. "alle ARM7-Kanäle bereit"). Mögliche Fehlerquelle!
 *   FEHLT      – noch nicht implementiert, Zugriff wird nur verworfen/protokolliert
 * Der Bericht listet alle benutzten Einträge. So sieht man bei Fehlern sofort,
 * welche Vereinfachungen im Spiel waren.
 */
typedef enum { ORIGINAL, SIMULIERT, FEHLT } HleCat;
static const char *const hle_cat_name[] = { "ORIGINAL ", "SIMULIERT", "FEHLT    " };
#define HLE_MAX 128
static struct { HleCat cat; const char *what; uint64_t n; } hle[HLE_MAX];

static void hle_hit(HleCat cat, const char *what) {
    for (int i = 0; i < HLE_MAX; i++) {
        if (!hle[i].what) { hle[i].cat = cat; hle[i].what = what; hle[i].n = 1; return; }
        if (hle[i].what == what || strcmp(hle[i].what, what) == 0) { hle[i].n++; return; }
    }
}
#define HLE(cat, what) hle_hit(cat, what)

/* ================================================================ Speicher */

static uint8_t itcm[0x8000];
static uint8_t ram[0x400000];
static uint8_t swram[0x8000];
static uint8_t dtcm[0x4000];
static uint8_t io[0x2000];
static uint8_t pal[0x800];
static uint8_t vram[0x900000];
static uint8_t oam[0x800];

static uint64_t mem_accesses, mem_budget;
static jmp_buf stop_jmp;
static const char *stop_reason = "?";

static void stop(const char *why) {
    stop_reason = why;
    longjmp(stop_jmp, 1);
}

static inline uint8_t *ptr(uint32_t a) {
    switch (a >> 24) {
    case 0x01: return &itcm[a & 0x7fff];
    case 0x02:
        if ((a & 0xffffc000u) == 0x027e0000u) return &dtcm[a & 0x3fff];
        return &ram[a & 0x3fffff];
    case 0x03: return &swram[a & 0x7fff];
    case 0x05: return &pal[a & 0x7ff];
    case 0x06: { uint32_t o = a & 0xffffff; return o < sizeof vram ? &vram[o] : NULL; }
    case 0x07: return &oam[a & 0x7ff];
    default: return NULL;
    }
}

/* ---- I/O-Statistik */
#define IO_STAT 4096
static struct { uint32_t addr; uint64_t rd, wr; } io_stat[IO_STAT];
static void io_count(uint32_t a, int write) {
    uint32_t h = (a * 2654435761u) % IO_STAT;
    for (int i = 0; i < IO_STAT; i++) {
        uint32_t k = (h + i) % IO_STAT;
        if (io_stat[k].addr == a || io_stat[k].addr == 0) {
            io_stat[k].addr = a;
            if (write) io_stat[k].wr++; else io_stat[k].rd++;
            return;
        }
    }
}

static uint32_t unmapped_count;
static void unmapped(const char *op, uint32_t a) {
    if ((a >> 24) == 0x08 || (a >> 24) == 0x09 || (a >> 24) == 0x0a) {
        HLE(SIMULIERT, "GBA-Slot leer (liefert 0 statt Open-Bus)");
        return;
    }
    HLE(FEHLT, "Zugriff auf nicht nachgebildeten Speicher");
    if (unmapped_count++ < 15) fprintf(stderr, "  [mem] %s ungemappt 0x%08x\n", op, a);
}

/* ---- I/O-Register */
static inline uint16_t io16(uint32_t off) { return (uint16_t)(io[off] | io[off + 1] << 8); }
static inline uint32_t io32(uint32_t off) { return io16(off) | (uint32_t)io16(off + 2) << 16; }
static inline void set_io16(uint32_t off, uint16_t v) { io[off] = (uint8_t)v; io[off + 1] = v >> 8; }
static inline void set_io32(uint32_t off, uint32_t v) { set_io16(off, (uint16_t)v); set_io16(off + 2, v >> 16); }

static uint16_t vcount;
static uint16_t timer_cnt[4];

static void hw_div(void) {
    uint32_t cnt = io16(0x280) & 3;
    int64_t num, den;
    if (cnt == 0) { num = (int32_t)io32(0x290); den = (int32_t)io32(0x298); }
    else { num = (int64_t)(io32(0x290) | (uint64_t)io32(0x294) << 32);
           den = cnt == 1 ? (int32_t)io32(0x298) : (int64_t)(io32(0x298) | (uint64_t)io32(0x29c) << 32); }
    int64_t q, r;
    if (den == 0) { q = num < 0 ? 1 : -1; r = num; }
    else if (num == INT64_MIN && den == -1) { q = num; r = 0; }
    else { q = num / den; r = num % den; }
    set_io32(0x2a0, (uint32_t)q); set_io32(0x2a4, (uint32_t)((uint64_t)q >> 32));
    set_io32(0x2a8, (uint32_t)r); set_io32(0x2ac, (uint32_t)((uint64_t)r >> 32));
}

static void hw_sqrt(void) {
    uint64_t v = (io16(0x2b0) & 1) ? (io32(0x2b8) | (uint64_t)io32(0x2bc) << 32) : io32(0x2b8);
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    set_io32(0x2b4, (uint32_t)r);
}

static uint64_t dma_count;
static void dma_run(int ch) {
    uint32_t base = 0xb0 + ch * 12;
    uint32_t src = io32(base), dst = io32(base + 4), cnt = io32(base + 8);
    if (!(cnt & 0x80000000u)) return;
    uint32_t timing = (cnt >> 27) & 7;
    if (timing != 0) {         /* nur "sofort" wird ausgeführt */
        HLE(FEHLT, "DMA mit Start bei VBlank/HBlank/Karte/GX-FIFO");
        return;
    }
    HLE(ORIGINAL, "DMA sofort");
    uint32_t n = cnt & 0x1fffff; if (!n) n = 0x200000;
    int w32 = (cnt >> 26) & 1;
    int dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
    int step = w32 ? 4 : 2;
    for (uint32_t i = 0; i < n; i++) {
        if (w32) wr32(dst, rd32(src)); else wr16(dst, rd16(src));
        if (sctl == 0) src += step; else if (sctl == 1) src -= step;
        if (dctl == 0 || dctl == 3) dst += step; else if (dctl == 1) dst -= step;
    }
    dma_count++;
    set_io32(base + 8, cnt & ~0x80000000u);
}

static uint32_t io_read(uint32_t a, int size) {
    io_count(a, 0);
    if (a >= 0x04100000u) {                         /* IPC-FIFO-Empfang, Karte */
        HLE(FEHLT, a == 0x04100000u ? "ARM7: IPC-FIFO-Empfang (keine Antworten)" : "Karten-Datenport");
        return 0;
    }
    uint32_t off = a & 0x1fff;
    switch (off) {
    case 0x004: io[4] ^= 1; HLE(SIMULIERT, "DISPSTAT: VBlank-Flag wechselt bei jedem Lesen"); break;
    case 0x006: vcount = (vcount + 1) % 263; set_io16(6, vcount);
        HLE(SIMULIERT, "VCOUNT zaehlt pro Lesezugriff"); break;
    case 0x100: case 0x104: case 0x108: case 0x10c:
        set_io16(off, timer_cnt[(off - 0x100) / 4] += 64);
        HLE(SIMULIERT, "Timer zaehlen pro Lesezugriff statt nach Zeit"); break;
    case 0x130: set_io16(0x130, 0x03ff); HLE(SIMULIERT, "Tasten: keine gedrueckt"); break;
    case 0x184: set_io16(0x184, (io16(0x184) & 0x8404) | 0x0101);
        HLE(SIMULIERT, "ARM7: IPC-FIFOs immer leer"); break;
    case 0x180: {                                   /* IPCSYNC: ARM7-Ersatz für den PXI-Handshake */
        /* PXI_InitFifo (0x0203c080) zählt mit dem ARM7 herunter: ARM9 schreibt den
         * gelesenen Wert zurück, der ARM7 antwortet mit "Wert - 1". Das reicht für
         * den Handshake, ist aber nicht aus dem ARM7-Code rekonstruiert. */
        uint16_t v = io16(0x180);
        set_io16(0x180, (uint16_t)((v & 0xff00) | (((v >> 8) - 1) & 0x0f)));
        HLE(SIMULIERT, "ARM7: PXI-Handshake ueber IPCSYNC (Wert - 1)");
        break; }
    case 0x280: case 0x2b0: io[off + 1] &= 0x7f; break; /* Divider/Sqrt nie beschäftigt */
    }
    if (size == 1) return io[off];
    if (size == 2) return io16(off & ~1u);
    return io32(off & ~3u);
}

static void io_write(uint32_t a, uint32_t v, int size) {
    io_count(a, 1);
    if (a >= 0x04100000u) return;
    uint32_t off = a & 0x1fff;
    if (off == 0x188) HLE(FEHLT, "ARM7: IPC-FIFO-Senden (Nachrichten werden verworfen)");
    if ((off >= 0x290 && off < 0x2a0) || off == 0x280) HLE(ORIGINAL, "Hardware-Divider");
    if ((off >= 0x2b8 && off < 0x2c0) || off == 0x2b0) HLE(ORIGINAL, "Hardware-Wurzel");
    if (off >= 0x400 && off < 0x700) HLE(FEHLT, "3D-Grafik (GX-Befehle)");
    if (off == 0x214 && size >= 2) {                /* IF: 1 schreiben = löschen */
        uint32_t cur = io32(0x214);
        set_io32(0x214, cur & ~(size == 4 ? v : (v & 0xffff)));
        return;
    }
    if (size == 1) io[off] = (uint8_t)v;
    else if (size == 2) set_io16(off & ~1u, (uint16_t)v);
    else set_io32(off & ~3u, v);
    if (off >= 0x290 && off < 0x2a0) hw_div();
    if (off == 0x280) hw_div();
    if (off >= 0x2b8 && off < 0x2c0) hw_sqrt();
    if (off == 0x2b0) hw_sqrt();
    if (off >= 0xb8 && off < 0xe0) {
        int ch = (off - 0xb0) / 12;
        if ((off - 0xb0) % 12 >= 8) dma_run(ch);
    }
}

#define BUDGET() do { if (++mem_accesses > mem_budget) stop("Speicherzugriffs-Budget aufgebraucht"); } while (0)

uint8_t rd8(uint32_t a) {
    BUDGET();
    uint8_t *p = ptr(a); if (p) return *p;
    if ((a >> 24) == 4) return (uint8_t)io_read(a, 1);
    unmapped("rd8", a); return 0;
}
uint16_t rd16(uint32_t a) {
    BUDGET(); a &= ~1u;
    uint8_t *p = ptr(a); if (p) return (uint16_t)(p[0] | p[1] << 8);
    if ((a >> 24) == 4) return (uint16_t)io_read(a, 2);
    unmapped("rd16", a); return 0;
}
uint32_t rd32(uint32_t a) {
    BUDGET();
    uint32_t rot = (a & 3) * 8; a &= ~3u;
    uint32_t v;
    uint8_t *p = ptr(a);
    if (p) v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    else if ((a >> 24) == 4) v = io_read(a, 4);
    else { unmapped("rd32", a); v = 0; }
    return ROR32(v, rot);
}
void wr8(uint32_t a, uint32_t v) {
    BUDGET();
    uint8_t *p = ptr(a); if (p) { *p = (uint8_t)v; return; }
    if ((a >> 24) == 4) { io_write(a, v, 1); return; }
    unmapped("wr8", a);
}
void wr16(uint32_t a, uint32_t v) {
    BUDGET(); a &= ~1u;
    uint8_t *p = ptr(a); if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return; }
    if ((a >> 24) == 4) { io_write(a, v, 2); return; }
    unmapped("wr16", a);
}
void wr32(uint32_t a, uint32_t v) {
    BUDGET(); a &= ~3u;
    uint8_t *p = ptr(a);
    if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); return; }
    if ((a >> 24) == 4) { io_write(a, v, 4); return; }
    unmapped("wr32", a);
}

/* ================================================================ Funktionen */

typedef struct {
    uint32_t addr; int ov; RcFunc fn; const char *name; uint64_t calls;
    uint32_t first_ctx[12];   /* die 12 Funktionen vor dem ersten Aufruf */
} FnInfo;
static FnInfo *fns;
static unsigned nfns;

/* Overlay-Inhalte, um zu erkennen, welches Overlay gerade im RAM liegt */
static struct { uint32_t base; uint32_t size; uint8_t *data; } ovl[256];

static int cmp_fn(const void *a, const void *b) {
    const FnInfo *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int overlay_present(int id, uint32_t addr) {
    if (id < 0) return 1;
    if (!ovl[id].data || addr < ovl[id].base || addr + 16 > ovl[id].base + ovl[id].size) return 0;
    uint8_t *p = ptr(addr);
    return p && memcmp(p, ovl[id].data + (addr - ovl[id].base), 16) == 0;
}

static FnInfo *find_fn(uint32_t addr) {
    addr &= ~1u;
    unsigned lo = 0, hi = nfns;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (fns[m].addr < addr) lo = m + 1; else hi = m; }
    FnInfo *best = NULL;
    for (unsigned i = lo; i < nfns && fns[i].addr == addr; i++) {
        if (fns[i].ov < 0) return &fns[i];
        if (overlay_present(fns[i].ov, addr)) best = &fns[i];
    }
    return best;
}

static const char *fn_name(uint32_t addr) {
    FnInfo *f = find_fn(addr);
    return f ? f->name : "?";
}

static uint32_t ring[64];
static unsigned ring_pos;
static uint64_t fn_entries;
static int in_irq;
static void vblank(CPU *c);

#define OS_SLEEP_THREAD 0x02038dc4u

void rc_enter(CPU *c, uint32_t addr) {
    /* Vereinfachtes Thread-Modell: legt sich ein Thread schlafen, vergeht Zeit
     * bis zum nächsten VBlank. Der Interrupt-Handler des Spiels weckt ihn dann. */
    if (addr == OS_SLEEP_THREAD && !in_irq) {
        HLE(SIMULIERT, "Threads: Schlafen = Zeit bis zum naechsten Frame (kein Thread-Wechsel)");
        vblank(c);
    }
    ring[ring_pos++ & 63] = addr;
    fn_entries++;
    FnInfo *f = find_fn(addr);
    if (f) {
        if (f->calls++ == 0)
            for (int i = 0; i < 12; i++) f->first_ctx[i] = ring[(ring_pos - 13 + i) & 63];
    }
}

static uint64_t indirect_fail;
void rc_call_indirect(CPU *c, uint32_t addr) {
    FnInfo *f = find_fn(addr);
    if (!f) {
        if (indirect_fail++ < 10)
            fprintf(stderr, "  [call] kein Ziel fuer 0x%08x (aus %s)\n", addr, fn_name(ring[(ring_pos - 1) & 63]));
        if (addr == 0) stop("Aufruf ueber Nullzeiger");
        return;
    }
    f->fn(c);
}
void rc_jump_indirect(CPU *c, uint32_t addr) { rc_call_indirect(c, addr); }

static uint64_t fallthroughs;
void rc_fallthrough(CPU *c, uint32_t addr) {
    fallthroughs++;
    rc_call_indirect(c, addr);
}

#define TODO_MAX 64
static struct { const char *what; uint64_t n; } todos[TODO_MAX];
void rc_todo(CPU *c, const char *what) {
    (void)c;
    for (int i = 0; i < TODO_MAX; i++) {
        if (todos[i].what == what || !todos[i].what) { todos[i].what = what; todos[i].n++; return; }
    }
}

/* ================================================================ Interrupts */

#define IRQ_HANDLER_PTR 0x027e3ffcu
#define IRQ_CHECK_FLAGS 0x027e3ff8u
#define IRQ_STACK_TOP   0x027e3f80u

static uint64_t frames, frame_limit;

static void raise_irq(CPU *c, uint32_t bits) {
    static int shown;
    if (shown++ < 3)
        fprintf(stderr, "  [irq] IF|=%x  IME=%x IE=%08x Handler=%08x cpsr=%x\n", bits,
                io32(0x208), io32(0x210), rd32(IRQ_HANDLER_PTR), c->cpsr_ctrl);
    set_io32(0x214, io32(0x214) | bits);
    if (in_irq || !(io32(0x208) & 1) || !(io32(0x210) & bits)) return;
    uint32_t handler = rd32(IRQ_HANDLER_PTR);
    if (!handler) return;
    CPU saved = *c;
    in_irq = 1;
    c->r[13] = IRQ_STACK_TOP;
    c->r[14] = 0;
    rc_call_indirect(c, handler);
    in_irq = 0;
    *c = saved;
}

static void vblank(CPU *c) {
    frames++;
    if (frame_limit && frames >= frame_limit) stop("Frame-Limit erreicht");
    io[4] |= 1;
    /* Pro Frame: VCount-Match (Bit 2) und VBlank (Bit 0).
     * Genauer Zeitpunkt der VCount-Zeile wird noch nicht nachgebildet. */
    HLE(SIMULIERT, "Frame-Takt: VCount-Match + VBlank einmal pro Frame");
    raise_irq(c, 4);
    raise_irq(c, 1);
}

/* ================================================================ BIOS */

static uint64_t swi_count[0x40];

static void bios_lz77(uint32_t src, uint32_t dst) {
    uint32_t hdr = rd32(src); src += 4;
    uint32_t len = hdr >> 8, done = 0;
    while (done < len) {
        uint8_t flags = rd8(src++);
        for (int b = 0; b < 8 && done < len; b++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = rd8(src++), b2 = rd8(src++);
                uint32_t n = (b1 >> 4) + 3, disp = ((b1 & 0xf) << 8 | b2) + 1;
                for (uint32_t i = 0; i < n && done < len; i++, done++) wr8(dst + done, rd8(dst + done - disp));
            } else {
                wr8(dst + done++, rd8(src++));
            }
        }
    }
}

static void bios_rl(uint32_t src, uint32_t dst) {
    uint32_t len = rd32(src) >> 8, done = 0; src += 4;
    while (done < len) {
        uint8_t f = rd8(src++);
        if (f & 0x80) { uint32_t n = (f & 0x7f) + 3; uint8_t v = rd8(src++); while (n-- && done < len) wr8(dst + done++, v); }
        else { uint32_t n = (f & 0x7f) + 1; while (n-- && done < len) wr8(dst + done++, rd8(src++)); }
    }
}

void rc_swi(CPU *c, uint32_t n) {
    if (n > 0xff) n >>= 16;                         /* ARM-Modus: Nummer im oberen Byte */
    swi_count[n & 0x3f]++;
    static const char *const swi_name[0x20] = {
        [0x03] = "BIOS WaitByLoop", [0x04] = "BIOS IntrWait", [0x05] = "BIOS VBlankIntrWait",
        [0x06] = "BIOS Halt", [0x09] = "BIOS Div", [0x0b] = "BIOS CpuSet", [0x0c] = "BIOS CpuFastSet",
        [0x0d] = "BIOS Sqrt", [0x0f] = "BIOS IsDebugger", [0x11] = "BIOS LZ77UnComp (normal)",
        [0x14] = "BIOS RLUnComp (normal)",
    };
    if (n < 0x20 && swi_name[n]) HLE(n == 0x03 || n == 0x06 ? SIMULIERT : ORIGINAL, swi_name[n]);
    switch (n) {
    case 0x03: break;                               /* WaitByLoop */
    case 0x04: case 0x05: {                         /* IntrWait / VBlankIntrWait */
        uint32_t discard = n == 5 ? 1 : c->r[0];
        uint32_t mask = n == 5 ? 1 : c->r[1];
        if (discard) wr32(IRQ_CHECK_FLAGS, rd32(IRQ_CHECK_FLAGS) & ~mask);
        for (int i = 0; i < 1000; i++) {
            uint32_t f = rd32(IRQ_CHECK_FLAGS);
            if (f & mask) { wr32(IRQ_CHECK_FLAGS, f & ~mask); return; }
            if (mask & 1) vblank(c);
            else stop("IntrWait auf einen Interrupt ausser VBlank");
        }
        stop("IntrWait: Interrupt-Handler setzt die Flags nicht");
    }
    case 0x06: vblank(c); break;                    /* Halt */
    case 0x09: {                                    /* Div */
        int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1];
        if (b == 0) break;
        c->r[0] = (uint32_t)(a / b); c->r[1] = (uint32_t)(a % b);
        c->r[3] = (uint32_t)abs(a / b);
        break; }
    case 0x0b: case 0x0c: {                         /* CpuSet / CpuFastSet */
        uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
        uint32_t cnt = ctl & 0x1fffff;
        int fill = (ctl >> 24) & 1, w32 = n == 0x0c || ((ctl >> 26) & 1);
        int step = w32 ? 4 : 2;
        uint32_t v = w32 ? rd32(src) : rd16(src);
        for (uint32_t i = 0; i < cnt; i++) {
            if (!fill) v = w32 ? rd32(src + i * step) : rd16(src + i * step);
            if (w32) wr32(dst + i * step, v); else wr16(dst + i * step, v);
        }
        break; }
    case 0x0d: {                                    /* Sqrt */
        uint32_t v = c->r[0], r = 0;
        for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
            if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        }
        c->r[0] = r;
        break; }
    case 0x0f: c->r[0] = 0; break;                  /* IsDebugger */
    case 0x11: bios_lz77(c->r[0], c->r[1]); break;  /* LZ77UnCompReadNormalWrite8bit */
    case 0x14: bios_rl(c->r[0], c->r[1]); break;    /* RLUnCompReadNormalWrite8bit */
    default:
        HLE(FEHLT, "BIOS-Aufruf ohne Implementierung (Callback-Dekompression, CRC16, ...)");
        break;
    }
}

uint32_t rc_mrs(CPU *c, int spsr) {
    (void)spsr;
    HLE(SIMULIERT, "CPSR lesen: nur gespeicherter Wert, Flags getrennt");
    return c->cpsr_ctrl;
}
void rc_msr(CPU *c, const char *field, uint32_t v) {
    (void)field;
    HLE(SIMULIERT, "CPSR schreiben: kein echter Moduswechsel (gemeinsamer Stack)");
    c->cpsr_ctrl = v;
}
uint32_t rc_mrc(CPU *c, int a, int b, int d, int e) {
    (void)c; (void)a; (void)b; (void)d; (void)e;
    HLE(SIMULIERT, "CP15 lesen (liefert 0)");
    return 0;
}
void rc_mcr(CPU *c, int a, int b, int d, int e, uint32_t v) {
    (void)c; (void)a; (void)b; (void)d; (void)e; (void)v;
    HLE(SIMULIERT, "CP15 schreiben (Cache/MPU/TCM ohne Wirkung)");
}

/* ================================================================ Laden */

static long load_file(const char *dir, const char *name, uint8_t *dst, long max) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Kann %s nicht oeffnen\n", path); exit(2); }
    long n = (long)fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n;
}

static void load_overlays(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/arm9_overlays/overlays.yaml", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int id = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if (sscanf(line, " - id: %u", &v) == 1) id = (int)v;
        else if (id >= 0 && id < 256 && sscanf(line, " base_address: %u", &v) == 1) {
            ovl[id].base = v;
            char name[64];
            snprintf(name, sizeof name, "arm9_overlays/ov%03d.bin", id);
            ovl[id].data = malloc(0x100000);
            ovl[id].size = (uint32_t)load_file(dir, name, ovl[id].data, 0x100000);
        }
    }
    fclose(f);
}

/* ================================================================ Bericht */

static uint32_t ctx_addr;   /* optional: Funktion, deren Vorgeschichte gezeigt wird */

static int cmp_calls(const void *a, const void *b) {
    const FnInfo *x = *(FnInfo *const *)a, *y = *(FnInfo *const *)b;
    return x->calls < y->calls ? 1 : x->calls > y->calls ? -1 : 0;
}

static void report(void) {
    printf("\n==== Ergebnis ====\n");
    printf("Abbruch:            %s\n", stop_reason);
    printf("Frames (VBlank):    %llu\n", (unsigned long long)frames);
    printf("Funktionsaufrufe:   %llu\n", (unsigned long long)fn_entries);
    printf("Speicherzugriffe:   %llu\n", (unsigned long long)mem_accesses);
    printf("DMA-Kopien:         %llu\n", (unsigned long long)dma_count);
    printf("Fall-throughs:      %llu\n", (unsigned long long)fallthroughs);
    printf("Ungemappt:          %u\n", unmapped_count);
    printf("Indirekt ohne Ziel: %llu\n", (unsigned long long)indirect_fail);

    printf("\nLetzte Funktionen (neueste zuletzt):\n");
    for (unsigned i = 0; i < 24; i++) {
        uint32_t a = ring[(ring_pos - 24 + i) & 63];
        if (a) printf("  0x%08x  %s\n", a, fn_name(a));
    }

    FnInfo **sorted = malloc(nfns * sizeof *sorted);
    unsigned used = 0;
    for (unsigned i = 0; i < nfns; i++) if (fns[i].calls) sorted[used++] = &fns[i];
    qsort(sorted, used, sizeof *sorted, cmp_calls);
    printf("\nVerschiedene Funktionen ausgefuehrt: %u\nHaeufigste:\n", used);
    for (unsigned i = 0; i < used && i < 15; i++)
        printf("  %10llu  0x%08x  %s\n", (unsigned long long)sorted[i]->calls, sorted[i]->addr, sorted[i]->name);
    FnInfo *ctx_fn = ctx_addr ? find_fn(ctx_addr) : (used ? sorted[0] : NULL);
    if (ctx_fn && ctx_fn->calls) {
        printf("\nWas vor dem ersten Aufruf von %s lief:\n", ctx_fn->name);
        for (int i = 0; i < 12; i++)
            if (ctx_fn->first_ctx[i])
                printf("  0x%08x  %s\n", ctx_fn->first_ctx[i], fn_name(ctx_fn->first_ctx[i]));
    }
    free(sorted);

    printf("\nI/O-Register (haeufigste):\n");
    for (int k = 0; k < 20; k++) {
        int best = -1; uint64_t bn = 0;
        for (int i = 0; i < IO_STAT; i++) {
            uint64_t n = io_stat[i].rd + io_stat[i].wr;
            if (io_stat[i].addr && n > bn) { bn = n; best = i; }
        }
        if (best < 0) break;
        printf("  0x%08x  lesen %10llu  schreiben %10llu\n", io_stat[best].addr,
               (unsigned long long)io_stat[best].rd, (unsigned long long)io_stat[best].wr);
        io_stat[best].rd = io_stat[best].wr = 0;
        io_stat[best].addr |= 0;   /* bleibt belegt */
        io_stat[best].rd = io_stat[best].wr = 0;
    }

    printf("\nBIOS-Aufrufe:");
    for (int i = 0; i < 0x40; i++) if (swi_count[i]) printf(" 0x%02x:%llu", i, (unsigned long long)swi_count[i]);
    printf("\n\nHardware-Ersatz (HLE) nach Kategorie:\n");
    for (int cat = ORIGINAL; cat <= FEHLT; cat++)
        for (int i = 0; i < HLE_MAX && hle[i].what; i++)
            if ((int)hle[i].cat == cat)
                printf("  %s %10llu  %s\n", hle_cat_name[cat], (unsigned long long)hle[i].n, hle[i].what);
    printf("\nRecompiler-TODOs (nicht uebersetzte Befehle):\n");
    for (int i = 0; i < TODO_MAX && todos[i].what; i++)
        printf("  %8llu  %s\n", (unsigned long long)todos[i].n, todos[i].what);
}

/* ================================================================ Start */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: %s <extract/eur> [frames] [budget_mio]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    frame_limit = argc > 2 ? strtoull(argv[2], NULL, 0) : 600;
    mem_budget = (argc > 3 ? strtoull(argv[3], NULL, 0) : 2000) * 1000000ull;
    ctx_addr = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 16) : 0;

    /* Funktionstabelle sortieren */
    nfns = rc_functable_count;
    fns = calloc(nfns, sizeof *fns);
    for (unsigned i = 0; i < nfns; i++) {
        fns[i].addr = rc_functable[i].addr; fns[i].ov = rc_functable[i].overlay;
        fns[i].fn = rc_functable[i].fn;     fns[i].name = rc_functable[i].name;
    }
    qsort(fns, nfns, sizeof *fns, cmp_fn);

    /* Abbilder laden (entspricht Autoload + BSS-Löschen aus Entry) */
    load_file(dir, "arm9/arm9.bin", &ram[0x4000], sizeof ram - 0x4000);
    load_file(dir, "arm9/itcm.bin", itcm, sizeof itcm);
    load_file(dir, "arm9/dtcm.bin", dtcm, sizeof dtcm);
    load_overlays(dir);

    /* Was Entry an Hardware-Einstellungen macht */
    wr32(IRQ_HANDLER_PTR, 0x01ff8d70u);   /* OS-Interrupt-Handler im ITCM */
    wr32(0x027fff9cu, 0);
    /* ARM7-Ersatz: alle PXI-Kanäle des ARM7 gelten als bereit
     * (PXI_IsCallbackReady prüft 0x027ffc00 + 0x388 + proc*4) */
    wr32(0x027ffc00u + 0x388 + 4, 0xffffffffu);
    HLE(SIMULIERT, "ARM7: alle PXI-Kanaele als bereit gemeldet (ohne echte Gegenstelle)");
    set_io16(0x130, 0x03ff);

    static CPU c;
    memset(&c, 0, sizeof c);
    c.r[13] = 0x027e3c00u;                /* System-Stack im DTCM */
    c.cpsr_ctrl = 0x1f;                   /* System-Modus */

    printf("Funktionen: %u, Frame-Limit %llu, Budget %llu Mio. Speicherzugriffe\n",
           nfns, (unsigned long long)frame_limit, (unsigned long long)(mem_budget / 1000000));
    if (setjmp(stop_jmp) == 0) {
        /* Reihenfolge wie am Ende von Entry */
        rc_func_02045cb8(&c);
        rc_func_02019a18(&c);
        rc_func_020471dc(&c);
        rc_main(&c);
        stop_reason = "main ist zurueckgekehrt";
    }
    report();
    return 0;
}
