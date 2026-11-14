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
 *   - Threads: jeder SDK-Thread läuft in einem eigenen Fiber (sched.c,
 *     threads_hle.c); Interrupts nur an sicheren Punkten (deterministisch)
 *
 * Aufruf: boot_test.exe <extract/eur-Ordner> [frames] [budget_mio] [funktion_hex]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"
#include "funcs.h"
#include "platform.h"
#include "sched.h"
#include "arm7_hle.h"
#include "card.h"
#include "vram.h"
#include "render2d.h"
#include "gpu3d.h"
#include "png.h"
#ifdef PLATFORM_SDL
#include "frontend.h"
#endif

static uint32_t irq_pending_extra;   /* von Geräten angeforderte Interrupts (z. B. Karte fertig) */
#define IRQ_CARD_DONE (1u << 19)

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
static const char *const hle_cat_name[] = { "ORIGINAL ", "SIMULIERT", "FEHLT    " };
#define HLE_MAX 128
static struct { HleCat cat; const char *what; uint64_t n; } hle[HLE_MAX];

void hle_hit(HleCat cat, const char *what) {
    for (int i = 0; i < HLE_MAX; i++) {
        if (!hle[i].what) { hle[i].cat = cat; hle[i].what = what; hle[i].n = 1; return; }
        if (hle[i].what == what || strcmp(hle[i].what, what) == 0) { hle[i].n++; return; }
    }
}

/* ================================================================ Speicher */

static uint8_t itcm[0x8000];
static uint8_t ram[0x400000];
static uint8_t swram[0x8000];
static uint8_t dtcm[0x4000];
static uint8_t io[0x2000];
static uint8_t pal[0x800];
static uint8_t oam[0x800];

static uint64_t mem_accesses, mem_budget;
static const char *stop_reason = "?";
static CPU cpu;              /* der eine CPU-Kontext, den alle Threads teilen */

/* Beendet den Lauf aus jedem Thread: zurück in den Wurzel-Fiber, dann Bericht.
 * (longjmp wäre über Fiber-Grenzen hinweg nicht erlaubt.) */
static void stop(const char *why) {
    stop_reason = why;
    sched_stop(&cpu);
}
void platform_stop(const char *why) { stop(why); }

/* Zählerbasierter Takt: alle 2^22 Speicherzugriffe ein Frame, auch wenn kein
 * Thread schläft (für Warteschleifen, die ohne zu schlafen auf Interrupts warten).
 * Ausgeliefert wird er erst am nächsten sicheren Punkt (Funktionsanfang). */
#define TICK_MASK ((1ull << 22) - 1)
static int tick_pending;

static inline uint8_t *ptr(uint32_t a) {
    switch (a >> 24) {
    case 0x01: return &itcm[a & 0x7fff];
    case 0x02:
        if ((a & 0xffffc000u) == 0x027e0000u) return &dtcm[a & 0x3fff];
        return &ram[a & 0x3fffff];
    case 0x03: return &swram[a & 0x7fff];
    case 0x05: return &pal[a & 0x7ff];
    case 0x06: return vram_cpu_ptr(a);   /* Bänke A-I nach VRAMCNT */
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
    if ((a >> 24) == 0x06) {
        /* Bereich ohne zugeordnete VRAM-Bank: auch die Hardware ignoriert das */
        HLE(ORIGINAL, "VRAM-Zugriff ohne zugeordnete Bank (ignoriert wie Hardware)");
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
    if (getenv("BIS_DIV") && (q > 0x40000000ll || q < -0x40000000ll)) {
        static int shown;
        if (shown++ < 30) fprintf(stderr, "  [div] Modus %u: %lld / %lld = %lld  (DIVCNT %04x)\n", cnt, (long long)num, (long long)den, (long long)q, io16(0x280));
    }
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
static void dma_copy(int ch);
static uint64_t watch_frames(void);
static uint32_t ring_last(void);
static const char *fn_name(uint32_t addr);

/* Start-Zeitpunkt (Bits 27-29): 0 sofort, 1 VBlank, 2 HBlank, 5 Karte, 7 GX-FIFO */
static void dma_run(int ch) {
    uint32_t cnt = io32(0xb0 + ch * 12 + 8);
    if (!(cnt & 0x80000000u)) return;
    uint32_t timing = (cnt >> 27) & 7;
    if (timing == 0) { HLE(ORIGINAL, "DMA sofort"); dma_copy(ch); return; }
    if (timing == 7) {
        /* Hardware: Blöcke zu 112 Worten, sobald die GX-FIFO halb leer ist. Unsere
         * Geometrie-Engine nimmt alles sofort an, also läuft der DMA am Stück. */
        HLE(SIMULIERT, "DMA in die GX-FIFO: am Stueck statt in Bloecken");
        dma_copy(ch);
        return;
    }
    if (timing == 1 || timing == 5) return;   /* VBlank: in vblank(), Karte: dma_card() */
    HLE(FEHLT, "DMA mit Start bei HBlank/Hauptbildschirm/Hauptspeicher-Anzeige");
}

/* Interne Adresszähler je Kanal (ORIGINAL nach GBATEK) */
static uint32_t dma_isrc[4], dma_idst[4];
static void dma_latch(int ch) {
    uint32_t base = 0xb0 + (uint32_t)ch * 12;
    dma_isrc[ch] = io32(base); dma_idst[ch] = io32(base + 4);
}

static void dma_copy(int ch) {
    uint32_t base = 0xb0 + ch * 12;
    uint32_t cnt = io32(base + 8);
    if (((cnt >> 21) & 3) == 3) dma_idst[ch] = io32(base + 4);   /* Ziel "Inkrement+Neuladen" */
    uint32_t src = dma_isrc[ch], dst = dma_idst[ch];
    uint32_t n = cnt & 0x1fffff; if (!n) n = 0x200000;
    if (getenv("BIS_DMALOG")) {
        static int shown;
        if (shown++ < 40) fprintf(stderr, "  [dma] Kanal %d: %08x -> %08x, %u x %d Bytes, CNT %08x (Frame %llu)\n", ch, src, dst, n,
                                  (cnt >> 26) & 1 ? 4 : 2, cnt, (unsigned long long)watch_frames());
    }
    int w32 = (cnt >> 26) & 1;
    int dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
    int step = w32 ? 4 : 2;
    for (uint32_t i = 0; i < n; i++) {
        if (w32) wr32(dst, rd32(src)); else wr16(dst, rd16(src));
        if (sctl == 0) src += step; else if (sctl == 1) src -= step;
        if (dctl == 0 || dctl == 3) dst += step; else if (dctl == 1) dst -= step;
    }
    dma_isrc[ch] = src; dma_idst[ch] = dst;
    dma_count++;
    if (cnt & 0x40000000u) {   /* Bit 30: Interrupt am Ende (IF Bit 8 + Kanal) */
        HLE(ORIGINAL, "DMA-Ende-Interrupt");
        irq_pending_extra |= 1u << (8 + ch);
    }
    /* Repeat (Bit 25) bei VBlank-DMA: bleibt aktiv, startet beim nächsten VBlank erneut */
    if (!(((cnt >> 27) & 7) == 1 && ((cnt >> 25) & 1))) set_io32(base + 8, cnt & ~0x80000000u);
}

static void dma_vblank(void) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = io32(0xb0 + (uint32_t)ch * 12 + 8);
        if ((cnt & 0x80000000u) && ((cnt >> 27) & 7) == 1) { HLE(ORIGINAL, "DMA bei VBlank"); dma_copy(ch); }
    }
}

/* DMA mit Start "Karte" (ARM9-Timing 5): kopiert vom Datenport, solange die
 * Karte Daten liefert */
static void dma_card(void) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t base = 0xb0 + (uint32_t)ch * 12;
        uint32_t cnt = io32(base + 8);
        if (!(cnt & 0x80000000u) || ((cnt >> 27) & 7) != 5) continue;
        int dctl = (cnt >> 21) & 3;
        if (dctl == 3) dma_idst[ch] = io32(base + 4);
        uint32_t dst = dma_idst[ch];
        HLE(ORIGINAL, "DMA vom Karten-Datenport");
        while (card_transfer_active()) {
            wr32(dst, card_read_data());
            if (dctl == 0 || dctl == 3) dst += 4; else if (dctl == 1) dst -= 4;
        }
        dma_idst[ch] = dst;
        dma_count++;
        if (cnt & 0x40000000u) irq_pending_extra |= 1u << (8 + ch);
        if (!((cnt >> 25) & 1)) set_io32(base + 8, cnt & ~0x80000000u);   /* ohne Repeat: fertig */
        return;
    }
}

/* Karten-DMA dauert auf der Hardware: ein 0x200-Byte-Block braucht grob 100 µs. Der
 * Streaming-Lader (0x0201c9ac) verlässt sich vermutlich darauf: Er wertet noch Reste im
 * Ringpuffer aus, während der nächste Block unterwegs ist. Kopieren wir sofort, werden
 * diese Reste überschrieben (Hänger nach "Neues Spiel", siehe docs/grafik.md).
 * EXPERIMENTELL: BIS_CARD_DELAY=n verzögert die Übertragung um n Speicherzugriffe. Seit den
 * internen DMA-Adresszählern läuft das mit n = 2048 stabil (siehe docs/grafik.md); der
 * Standard ist noch 0 = sofort. */
static uint32_t card_countdown, card_delay = 0;
static int card_fire_pending;
static int dma_card_armed(void) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = io32(0xb0 + (uint32_t)ch * 12 + 8);
        if ((cnt & 0x80000000u) && ((cnt >> 27) & 7) == 5) return 1;
    }
    return 0;
}
static void card_fire(void);
static void card_schedule(void) {
    if (getenv("BIS_CARDLOG")) fprintf(stderr, "  [karte] planen? aktiv %d, DMA bereit %d, Countdown %u\n",
                                       card_transfer_active(), dma_card_armed(), card_countdown);
    if (card_transfer_active() && dma_card_armed() && !card_countdown && !card_fire_pending) {
        if (!card_delay) { card_fire(); return; }   /* BIS_CARD_DELAY=0: sofort (altes Verhalten) */
        card_countdown = card_delay;
        HLE(SIMULIERT, "Karten-DMA: Uebertragung nach fester Wartezeit (Speicherzugriffe statt Takt)");
    }
}
static void card_done_check(void);
static void card_fire(void) {
    card_fire_pending = 0;
    if (getenv("BIS_CARDLOG")) fprintf(stderr, "  [karte] DMA ausgeloest (aktiv %d, DMA bereit %d)\n", card_transfer_active(), dma_card_armed());
    platform_inhibit_irq++;
    dma_card();
    card_done_check();
    platform_inhibit_irq--;
}

/* GX-FIFO-Interrupt (IF Bit 21): GXSTAT Bits 30-31 = 1 "weniger als halb voll", 2 "leer".
 * Unsere Geometrie-Engine nimmt alles sofort an, die FIFO ist also immer leer und beide
 * Bedingungen gelten dauernd. Genutzt vom SDK für asynchrones Senden von 3D-Befehlen
 * (0x0203a598, Handler 0x0203a690). Pegelgesteuert wie auf der Hardware. */
static void gx_fifo_irq(void) {
    uint32_t mode = io32(0x600) >> 30;
    if (mode == 1 || mode == 2) {
        HLE(SIMULIERT, "3D: GX-FIFO-Interrupt (FIFO immer leer)");
        irq_pending_extra |= 1u << 21;
    }
}

static void card_done_check(void) {
    if (!card_transfer_active() && (io16(0x1a0) & 0x4000)) irq_pending_extra |= IRQ_CARD_DONE;
}

/* Tasten (aktiv-niedrig): KEYINPUT Bits 0-9 = A, B, Select, Start, rechts, links,
 * hoch, runter, R, L. X/Y liest das Spiel nicht aus einem Register, sondern aus
 * dem vom ARM7 gefüllten Puffer HW_BUTTON_XY_BUF (0x027fffa8): Bit 10 = X, Bit 11 = Y.
 * (Pad_ReadAndUpdate: (KEYINPUT | XY_BUF) ^ Maske) */
static uint16_t keys_in = 0x03ff;
static uint16_t keys_xy = 0x0c00;
#define HW_BUTTON_XY_BUF 0x027fffa8u

static uint32_t io_read_inner(uint32_t a, int size) {
    io_count(a, 0);
    if (a >= 0x04100000u) {                         /* IPC-FIFO-Empfang, Karte */
        if (a == 0x04100000u) {
            HLE(ORIGINAL, "IPC-FIFO-Empfang: Antwort des ARM7-Ersatzes lesen");
            return arm7_recv_pop();
        }
        if (a == 0x04100010u) {
            HLE(ORIGINAL, "Karten-Datenport lesen");
            uint32_t v = card_read_data();
            card_done_check();
            return v;
        }
        HLE(FEHLT, "Unbekanntes Register ab 0x04100000");
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
    case 0x130: set_io16(0x130, keys_in);
#ifdef PLATFORM_SDL
        HLE(ORIGINAL, "Tasten: Tastatur (SDL)");
#else
        HLE(SIMULIERT, "Tasten: keine gedrueckt");
#endif
        break;
    case 0x184:   /* IPCFIFOCNT: Senden nie voll, Empfang = Zustand der Antwort-Warteschlange */
        set_io16(0x184, (uint16_t)((io16(0x184) & 0x8404) | 0x0001 | (arm7_recv_empty() ? 0x0100 : 0)));
        HLE(ORIGINAL, "IPC-FIFO-Status (Senden nie voll)"); break;
    case 0x180: {                                   /* IPCSYNC: ARM7-Ersatz für den PXI-Handshake */
        /* PXI_InitFifo (0x0203c080) zählt mit dem ARM7 herunter: ARM9 schreibt den
         * gelesenen Wert zurück, der ARM7 antwortet mit "Wert - 1". Das reicht für
         * den Handshake, ist aber nicht aus dem ARM7-Code rekonstruiert. */
        uint16_t v = io16(0x180);
        set_io16(0x180, (uint16_t)((v & 0xff00) | (((v >> 8) - 1) & 0x0f)));
        HLE(SIMULIERT, "ARM7: PXI-Handshake ueber IPCSYNC (Wert - 1)");
        break; }
    case 0x280: case 0x2b0: io[off + 1] &= 0x7f; break; /* Divider/Sqrt nie beschäftigt */
    case 0x1a4: case 0x1a6: set_io32(0x1a4, card_read_romctrl()); break;
    }
    if (off >= 0x600 && off < 0x6a4) { gpu3d_regs(); HLE(ORIGINAL, "3D: Status und Testergebnisse lesen"); }
    if (size == 1) return io[off];
    if (size == 2) return io16(off & ~1u);
    return io32(off & ~3u);
}

static void io_write_inner(uint32_t a, uint32_t v, int size) {
    io_count(a, 1);
    if (a >= 0x04100000u) return;
    uint32_t off = a & 0x1fff;
    if (off == 0x188) {
        static int shown;
        HLE(ORIGINAL, "IPC-FIFO-Senden an den ARM7-Ersatz");
        /* NitroSDK-PXI: Bits 0-4 Kanal (Tag), Bit 5 Flag, Bits 6-31 Daten */
        if (shown++ < 20)
            fprintf(stderr, "  [pxi] an ARM7: Kanal %2u, Flag %u, Daten 0x%07x\n", v & 0x1f, (v >> 5) & 1, v >> 6);
        arm7_pxi_send(v);
    }
    if ((off >= 0x290 && off < 0x2a0) || off == 0x280) HLE(ORIGINAL, "Hardware-Divider");
    if ((off >= 0x2b8 && off < 0x2c0) || off == 0x2b0) HLE(ORIGINAL, "Hardware-Wurzel");
    if (off >= 0x400 && off < 0x600) {
        if (size == 4) { HLE(ORIGINAL, "3D: GX-Befehle (Geometrie-Engine)"); gpu3d_write(off, v); }
        else HLE(FEHLT, "3D: GX-Befehl mit 8/16-Bit-Zugriff");
        return;
    }
    if ((off & ~3u) == 0x600 && off + (uint32_t)size > 0x601 && ((size == 1 ? v << 8 : v) & 0x8000)) gpu3d_ack_error();
    if ((off & ~3u) == 0x600 && off + (uint32_t)size > 0x603) {   /* GXSTAT Bits 30-31: FIFO-Interrupt */
        uint32_t keep = io32(0x600) & ~0xc0000000u;
        uint32_t nv = size == 4 ? v : size == 2 ? v << 16 : v << 24;
        set_io32(0x600, keep | (nv & 0xc0000000u));
        gx_fifo_irq();
        return;
    }
    if (off >= 0x214 && off < 0x218) {              /* IF: 1 schreiben = löschen */
        uint32_t mask = (size == 4 ? v : size == 2 ? (v & 0xffff) : (v & 0xff)) << ((off - 0x214) * 8);
        set_io32(0x214, io32(0x214) & ~mask);
        /* Noch nicht ausgelieferte Geräte-Anforderungen gelten damit ebenfalls als gelöscht
         * (OS_ResetRequestIrqMask). Sonst feuert ein altes "Karte fertig" in die nächste
         * Übertragung und der Karten-Handler überspringt einen Block. Das setzt voraus, dass
         * die Übertragung Zeit braucht (card_delay > 0), wie auf der Hardware. */
        if (card_delay) irq_pending_extra &= ~mask;
        gx_fifo_irq();   /* pegelgesteuert: kommt sofort wieder, solange die Bedingung gilt */
        return;
    }
    int dma_ch = (off >= 0xb8 && off < 0xe0 && (off - 0xb0) % 12 >= 8) ? (int)((off - 0xb0) / 12) : -1;
    uint32_t dma_was_on = dma_ch >= 0 ? io32(0xb8 + (uint32_t)dma_ch * 12) & 0x80000000u : 0;
    if (size == 1) io[off] = (uint8_t)v;
    else if (size == 2) set_io16(off & ~1u, (uint16_t)v);
    else set_io32(off & ~3u, v);
    /* Hardware: Quelle/Ziel werden nur beim Einschalten (Bit 31: 0 -> 1) in die internen
     * Zähler übernommen. Das SDK stoppt Karten-DMA, indem es den laufenden Kanal kurz auf
     * "sofort" stellt; das eine Dummy-Wort geht dann an die weitergezählte Adresse. */
    if (dma_ch >= 0 && !dma_was_on && (io32(0xb8 + (uint32_t)dma_ch * 12) & 0x80000000u)) dma_latch(dma_ch);
    if (off >= 0x290 && off < 0x2a0) hw_div();
    if (off == 0x280) hw_div();
    if (off >= 0x2b8 && off < 0x2c0) hw_sqrt();
    if (off == 0x2b0) hw_sqrt();
    if (off >= 0xb8 && off < 0xe0) {
        int ch = (off - 0xb0) / 12;
        if ((off - 0xb0) % 12 >= 8) {
            if (getenv("BIS_CARDLOG")) fprintf(stderr, "  [dma] Kanal %d CNT = %08x (Schreiben %d Bytes auf %03x), SRC %08x DST %08x\n",
                                               ch, io32(0xb0 + (uint32_t)ch * 12 + 8), size, off, io32(0xb0 + (uint32_t)ch * 12), io32(0xb4 + (uint32_t)ch * 12));
            dma_run(ch);
            /* Karten-DMA kann auch nach dem Start der Karte eingerichtet werden */
            if (card_transfer_active()) card_schedule();
        }
    }
    /* VRAMCNT A-G (0x240-0x246), H/I (0x248/0x249); 0x247 ist WRAMCNT */
    for (uint32_t k = off & ~(uint32_t)(size - 1); k < (off & ~(uint32_t)(size - 1)) + (uint32_t)size; k++) {
        if (k >= 0x240 && k <= 0x246) { vram_set_cnt((int)(k - 0x240), io[k]); HLE(ORIGINAL, "VRAM-Baenke zuordnen (VRAMCNT)"); }
        if (k == 0x248 || k == 0x249) { vram_set_cnt((int)(k - 0x248 + 7), io[k]); HLE(ORIGINAL, "VRAM-Baenke zuordnen (VRAMCNT)"); }
    }
    if (off >= 0x1a8 && off < 0x1b0) card_write_cmd(off - 0x1a8, v, size);
    if ((off & ~3u) == 0x1a4 && (off + (uint32_t)size > 0x1a7)) {   /* oberes Byte (Start-Bit) geschrieben */
        /* Neue Übertragung: eine noch nicht ausgelieferte Fertigmeldung der vorigen
         * gehört nicht zu ihr (auf der Hardware kann die neue erst später kommen) */
        if (io[0x1a7] & 0x80) { irq_pending_extra &= ~IRQ_CARD_DONE; set_io32(0x214, io32(0x214) & ~IRQ_CARD_DONE); }
        set_io32(0x1a4, card_write_romctrl(io32(0x1a4)));
        if (dma_card_armed()) card_schedule();
        else card_done_check();
    }
}

/* I/O-Zugriffe (inkl. DMA und Karte) laufen am Stück, ohne Interrupt dazwischen */
static uint32_t io_read(uint32_t a, int size) {
    platform_inhibit_irq++;
    uint32_t v = io_read_inner(a, size);
    platform_inhibit_irq--;
    return v;
}
static void io_write(uint32_t a, uint32_t v, int size) {
    platform_inhibit_irq++;
    io_write_inner(a, v, size);
    platform_inhibit_irq--;
}

#define BUDGET() do { \
        if (++mem_accesses > mem_budget) stop("Speicherzugriffs-Budget aufgebraucht"); \
        if ((mem_accesses & TICK_MASK) == 0) tick_pending = 1; \
        if (card_countdown && !--card_countdown) card_fire(); \
    } while (0)

/* Interrupts auch in engen Warteschleifen ohne Funktionsaufruf: Steht etwas an,
 * wird es beim nächsten Lesezugriff des Spielcodes ausgeliefert (nie aus eigenem
 * Ersatz-Code heraus, nie mit gesperrten Interrupts, nie mit Thread-Wechsel). */
int platform_inhibit_irq;
static int in_irq;
static int sched_active;
static int deliver_pending(CPU *c);
static void vblank(CPU *c);
static uint64_t poll_irqs;

/* Welche Funktion lief, als in einer Warteschleife unterbrochen wurde? */
static struct { uint32_t addr; uint64_t n; } poll_where[16];
static uint32_t ring_last(void);

static void poll_deliver(void) {
    uint32_t w = ring_last();
    for (int i = 0; i < 16; i++)
        if (poll_where[i].addr == w || !poll_where[i].addr) { poll_where[i].addr = w; poll_where[i].n++; break; }
    platform_inhibit_irq++;
    if (deliver_pending(&cpu)) {
        poll_irqs++;
        HLE(SIMULIERT, "Interrupt in einer Warteschleife ausgeliefert (beim Lesezugriff)");
    } else if (tick_pending) {
        tick_pending = 0;
        HLE(SIMULIERT, "Zeit-Takt in einer Warteschleife (beim Lesezugriff)");
        vblank(&cpu);
    }
    platform_inhibit_irq--;
}

#define POLL_IRQ() do { \
        if (((irq_pending_extra & io32(0x210)) || tick_pending || !arm7_recv_empty()) && sched_active && !in_irq && \
            !platform_inhibit_irq && (io[0x208] & 1) && !(cpu.cpsr_ctrl & 0x80)) poll_deliver(); \
    } while (0)

/* Watchpoint (Fehlersuche): Schreibzugriffe auf [watch_lo, watch_hi) protokollieren */
static uint32_t watch_lo, watch_hi;
static uint32_t ctx_addr;   /* optional: Funktion, deren Vorgeschichte/Argumente gezeigt werden */
static uint32_t ctx_more[8];   /* weitere Adressen (Argument "a,b,c") */
static uint32_t ring_last(void);
static const char *fn_name(uint32_t addr);
static uint64_t watch_frames(void);
static uint32_t ring[64];      /* (vorläufige Definition, siehe unten) */
static unsigned ring_pos;
static void watch(uint32_t a, uint32_t v, int size) {
    static int shown;
    static long long from = -1;
    if (from < 0) from = getenv("BIS_WATCH_FROM") ? atoll(getenv("BIS_WATCH_FROM")) : 0;   /* ab Frame */
    if (a + (uint32_t)size <= watch_lo || a >= watch_hi || (long long)watch_frames() < from || shown++ >= 4000) return;
    fprintf(stderr, "  [watch] wr%d %08x = %08x  in %s  (Frame %llu, lr=%08x sp=%08x)  davor:", size * 8, a, v,
            fn_name(ring_last()), (unsigned long long)watch_frames(), cpu.r[14], cpu.r[13]);
    for (int i = 5; i >= 2; i--) fprintf(stderr, " %s", fn_name(ring[(ring_pos - i) & 63]));
    fprintf(stderr, "\n");
}
#define WATCH(a, v, s) do { if (watch_hi) watch(a, v, s); } while (0)

static uint32_t rwatch_addr;   /* Fehlersuche: BIS_RWATCH=adresse (Lesezugriffe auf dieses Wort) */
static void rwatch_hit(uint32_t a, int size);
static uint32_t rd_ring[16], rd_pos;   /* letzte Lesezugriffe außerhalb von Interrupts (Bericht) */
#define RWATCH(a, s) do { if (!in_irq) rd_ring[rd_pos++ & 15] = (a); \
    if (rwatch_addr && ((a) & ~3u) == rwatch_addr) rwatch_hit(a, s); } while (0)
uint8_t rd8(uint32_t a) {
    BUDGET(); POLL_IRQ(); RWATCH(a, 1);
    uint8_t *p = ptr(a); if (p) return *p;
    if ((a >> 24) == 4) return (uint8_t)io_read(a, 1);
    unmapped("rd8", a); return 0;
}
uint16_t rd16(uint32_t a) {
    BUDGET(); POLL_IRQ(); RWATCH(a, 2); a &= ~1u;
    uint8_t *p = ptr(a); if (p) return (uint16_t)(p[0] | p[1] << 8);
    if ((a >> 24) == 4) return (uint16_t)io_read(a, 2);
    unmapped("rd16", a); return 0;
}
static void rwatch_hit(uint32_t a, int size) {
    static int shown;
    static long long from = -1;
    if (from < 0) from = getenv("BIS_WATCH_FROM") ? atoll(getenv("BIS_WATCH_FROM")) : 0;   /* ab Frame */
    if ((long long)watch_frames() < from) return;
    if (shown++ < 40) fprintf(stderr, "  [rwatch] rd%d %08x in %s (Frame %llu, lr=%08x)\n", size * 8, a,
                              fn_name(ring_last()), (unsigned long long)watch_frames(), cpu.r[14]);
}
uint32_t rd32(uint32_t a) {
    BUDGET(); POLL_IRQ(); RWATCH(a, 4);
    uint32_t rot = (a & 3) * 8; a &= ~3u;
    uint32_t v;
    uint8_t *p = ptr(a);
    if (p) v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    else if ((a >> 24) == 4) v = io_read(a, 4);
    else { unmapped("rd32", a); v = 0; }
    return ROR32(v, rot);
}
void wr8(uint32_t a, uint32_t v) {
    BUDGET(); WATCH(a, v, 1);
    uint8_t *p = ptr(a); if (p) { *p = (uint8_t)v; return; }
    if ((a >> 24) == 4) { io_write(a, v, 1); return; }
    unmapped("wr8", a);
}
void wr16(uint32_t a, uint32_t v) {
    BUDGET(); a &= ~1u; WATCH(a, v, 2);
    uint8_t *p = ptr(a); if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return; }
    if ((a >> 24) == 4) { io_write(a, v, 2); return; }
    unmapped("wr16", a);
}
void wr32(uint32_t a, uint32_t v) {
    BUDGET(); a &= ~3u; WATCH(a, v, 4);
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
    if (f) return f->name;
    static char buf[4][16]; static int k;
    snprintf(buf[k & 3], sizeof buf[0], "?%08x", addr);
    return buf[k++ & 3];
}

static uint32_t ring[64];
static unsigned ring_pos;
static uint64_t fn_entries;
static void resched_if_pending(CPU *c);
static uint32_t last_outside_irq;   /* zuletzt betretene Funktion außerhalb von Interrupts */
static uint32_t ring_last(void) { return last_outside_irq; }

void rc_enter(CPU *c, uint32_t addr) {
    /* Antwort des ARM7 oder Geräte-Interrupt liegt bereit: am sicheren Punkt ausliefern */
    if (!in_irq && (!arm7_recv_empty() || (irq_pending_extra & io32(0x210))) && (io[0x208] & 1) && !(c->cpsr_ctrl & 0x80)) {
        if (deliver_pending(c)) resched_if_pending(c);
    }
    /* Sicherer Punkt für den zählerbasierten Takt: nur wenn Interrupts erlaubt
     * sind (IME an, I-Bit im CPSR aus) – so wie auf der echten Hardware. */
    if (tick_pending && !in_irq && (io[0x208] & 1) && !(c->cpsr_ctrl & 0x80)) {
        tick_pending = 0;
        HLE(SIMULIERT, "Zeit-Takt alle 2^22 Speicherzugriffe (an Funktionsanfaengen)");
        vblank(c);
        resched_if_pending(c);
    }
    ring[ring_pos++ & 63] = addr;
    if (!in_irq) last_outside_irq = addr;
    int ctx_hit = addr == ctx_addr;
    for (int k = 0; k < 8 && ctx_more[k] && !ctx_hit; k++) ctx_hit = addr == ctx_more[k];
    if (ctx_hit) {
        static int shown;
        if (shown++ < 100000) {
            fprintf(stderr, "  [args] %08x(r0=%08x r1=%08x r2=%08x r3=%08x) lr=%08x  Frame %llu  davor:",
                    addr, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], (unsigned long long)watch_frames());
            for (int i = 6; i >= 2; i--) fprintf(stderr, " %s", fn_name(ring[(ring_pos - i) & 63]));
            fprintf(stderr, "\n");
            /* BIS_CTXMEM="ac*:40,c00:10": Worte ab r0+off ausgeben ("*": erst Zeiger lesen) */
            platform_inhibit_irq++;
            for (const char *s = getenv("BIS_CTXMEM"); s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {
                char *e;
                uint32_t off = (uint32_t)strtoul(s, &e, 16), a = c->r[0] + off;
                if (*e == '*') { a = rd32(a); e++; }
                uint32_t n = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 16) : 0x10;
                fprintf(stderr, "        [%x%s] %08x:", off, strchr(s, '*') && strchr(s, '*') < e ? "*" : "", a);
                for (uint32_t i = 0; i < n; i += 4) fprintf(stderr, " %08x", rd32(a + i));
                fprintf(stderr, "\n");
            }
            platform_inhibit_irq--;
        }
    }
    fn_entries++;
    FnInfo *f = find_fn(addr);
    if (f) {
        if (f->calls++ == 0)
            for (int i = 0; i < 12; i++) f->first_ctx[i] = ring[(ring_pos - 13 + i) & 63];
    }
}

int platform_is_function(uint32_t addr) { return find_fn(addr) != NULL; }

static uint64_t indirect_fail;
void rc_call_indirect(CPU *c, uint32_t addr) {
    FnInfo *f = find_fn(addr);
    if (!f) {
        if (indirect_fail++ < 10) {
            fprintf(stderr, "  [call] kein Ziel fuer 0x%08x (aus %s)\n", addr, fn_name(ring[(ring_pos - 1) & 63]));
            fprintf(stderr, "         r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n",
                    c->r[0], c->r[1], c->r[2], c->r[3], c->r[13], c->r[14]);
        }
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
static uint64_t watch_frames(void) { return frames; }

/* Läuft innerhalb von sched_irq: CPU-Kontext wird davor gesichert und danach
 * wiederhergestellt, ein Thread-Wechsel im Interrupt wäre ein Verstoß. */
static void irq_entry(CPU *c, void *arg) {
    uint32_t handler = (uint32_t)(uintptr_t)arg;
    in_irq = 1;
    c->r[13] = IRQ_STACK_TOP;
    c->r[14] = 0;
    c->cpsr_ctrl = 0x92;      /* IRQ-Modus, weitere IRQs gesperrt */
    rc_call_indirect(c, handler);
    in_irq = 0;
}

/* force = Interrupt auch bei gesetztem I-Bit ausliefern (nur für den simulierten
 * Leerlauf in OS_SelectThread, der innerhalb einer gesperrten Sektion läuft) */
static int irq_force;

static void raise_irq(CPU *c, uint32_t bits) {
    set_io32(0x214, io32(0x214) | bits);
    if (in_irq || !(io32(0x208) & 1) || !(io32(0x210) & bits)) return;
    if ((c->cpsr_ctrl & 0x80) && !irq_force) return;   /* I-Bit: Interrupts gesperrt */
    platform_inhibit_irq++;
    uint32_t handler = rd32(IRQ_HANDLER_PTR);
    if (handler) sched_irq(c, bits, irq_entry, (void *)(uintptr_t)handler);
    platform_inhibit_irq--;
}

#define IRQ_IPC_RECV (1u << 18)   /* IPC-Empfangs-FIFO nicht leer */

/* Liefert anstehende Interrupts aus, die nicht vom Frame-Takt kommen
 * (bisher: Antworten des ARM7). Rückgabe 1, wenn etwas ausgeliefert wurde. */
static int deliver_pending(CPU *c) {
    uint32_t bits = (arm7_recv_empty() ? 0 : IRQ_IPC_RECV) | irq_pending_extra;
    bits &= io32(0x210);
    if (!bits) return 0;
    irq_pending_extra &= ~bits;
    raise_irq(c, bits);
    return 1;
}

/* ---- Bild ausgeben: beide Engines als PNG (oben/unten nach POWCNT1 Bit 15) */
static char frames_dir[1024];
static uint64_t dump_every = 100;

static void dump_frame(const char *tag) {
    static uint32_t top[256 * 192], bottom[256 * 192], both[256 * 384];
    static int warned;
    Gfx2D g = { io, pal, oam, gpu3d_frame() };
    unsigned missing = 0;
    int a_on_top = (io16(0x304) >> 15) & 1;
    render2d_engine(&g, a_on_top ? 0 : 1, top, 1, &missing);
    render2d_engine(&g, a_on_top ? 1 : 0, bottom, 1, &missing);
    memcpy(both, top, sizeof top);
    memcpy(both + 256 * 192, bottom, sizeof bottom);
    char path[1200];
    snprintf(path, sizeof path, "%s/frame_%s.png", frames_dir, tag);
    if (!png_write_rgb(path, 256, 384, both) && !warned++) fprintf(stderr, "  [bild] kann %s nicht schreiben\n", path);
    if (getenv("BIS_3DPNG")) {   /* Fehlersuche: reines 3D-Bild (Alpha 0 = magenta) */
        const Px3D *d = gpu3d_frame();
        for (int i = 0; i < 256 * 192; i++) {
            uint16_t c = d[i].color;
            uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
            top[i] = d[i].alpha ? r << 16 | g << 8 | b : 0xff00ff;
        }
        snprintf(path, sizeof path, "%s/frame_%s_3d.png", frames_dir, tag);
        png_write_rgb(path, 256, 192, top);
    }
    static const char *const miss_name[] = {
        "Renderer: affine/erweiterte Hintergruende", "Renderer: 3D-Ebene (BG0 von Engine A)",
        "Renderer: affine Sprites", "Renderer: Bitmap-Sprites", "Renderer: Fenster",
        "Renderer: Alpha-Blending", "Renderer: Anzeigemodus 2/3 (VRAM/Hauptspeicher)", "Renderer: Mosaik" };
    for (int i = 0; i < 8; i++) if (missing & (1u << i)) HLE(FEHLT, miss_name[i]);
    HLE(ORIGINAL, "Renderer: Text-Hintergruende, Sprites, Paletten, Helligkeit, Blending");
    static const char *const g3d_name[] = {
        "3D: Schatten-Polygone", "3D: Nebel", "3D: Kantenmarkierung", "3D: Anti-Aliasing",
        "3D: Texturkoordinaten aus Normale/Vertex (Skalierung ungeprueft)", "3D: Rear-Plane-Bitmap",
        "3D: Box-Test (meldet immer sichtbar)", "3D: Polygon-/Vertex-Speicher voll" };
    unsigned gf = gpu3d_flags();
    for (int i = 0; i < 8; i++) if (gf & (1u << i)) HLE(i == 6 ? SIMULIERT : FEHLT, g3d_name[i]);
}

void platform_idle(CPU *c) {
    irq_force = 1;
    HLE(SIMULIERT, "Leerlauf in OS_SelectThread: Interrupt trotz gesperrtem I-Bit");
    if (card_countdown || card_fire_pending) {   /* alle warten: die Karten-Übertragung wird fertig */
        card_countdown = 0;
        card_fire();
    }
    if (!deliver_pending(c)) vblank(c);
    irq_force = 0;
}

/* Interrupt hat einen Thread geweckt: wie beim Verlassen eines echten IRQs neu
 * planen. Der unterbrochene Thread wird komplett gesichert. */
static void resched_if_pending(CPU *c) {
    if (!platform_resched_pending) return;
    platform_resched_pending = 0;
    CPU keep = *c;
    rc_OS_RescheduleThread(c);
    *c = keep;
}

/* ---- Eingabe-Automatik (Tests ohne Fenster), Umgebungsvariable BIS_INPUT:
 *   "1100:start,1300:a,1500:a"  -> Taste ab diesem Frame für 6 Frames gedrückt
 * Tasten: a b x y start select up down left right l r. Reproduzierbar, da an Frames gebunden. */
static struct { uint64_t frame; uint16_t keybit; int xy; } script[64];
static int script_n;

static void script_load(void) {
    if (getenv("BIS_RWATCH")) rwatch_addr = (uint32_t)strtoul(getenv("BIS_RWATCH"), NULL, 16) & ~3u;
    if (getenv("BIS_LAYERS")) render2d_layer_mask = (unsigned)strtoul(getenv("BIS_LAYERS"), NULL, 16);
    const char *s = getenv("BIS_INPUT");
    static const struct { const char *name; uint16_t bit; int xy; } names[] = {
        { "a", 1 << 0, 0 }, { "b", 1 << 1, 0 }, { "select", 1 << 2, 0 }, { "start", 1 << 3, 0 },
        { "right", 1 << 4, 0 }, { "left", 1 << 5, 0 }, { "up", 1 << 6, 0 }, { "down", 1 << 7, 0 },
        { "r", 1 << 8, 0 }, { "l", 1 << 9, 0 }, { "x", 0x400, 1 }, { "y", 0x800, 1 } };
    while (s && *s && script_n < 64) {
        char key[16] = { 0 };
        unsigned long long f;
        if (sscanf(s, "%llu:%15[a-z]", &f, key) != 2) break;
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(key, names[i].name)) { script[script_n].frame = f; script[script_n].keybit = names[i].bit; script[script_n].xy = names[i].xy; script_n++; }
        s = strchr(s, ',');
        if (s) s++;
    }
}

static void script_apply(void) {
    if (!script_n) return;
    keys_in = 0x03ff;
    keys_xy = 0x0c00;
    for (int i = 0; i < script_n; i++) {
        if (frames >= script[i].frame && frames < script[i].frame + 6) {
            if (script[i].xy) keys_xy &= (uint16_t)~script[i].keybit; else keys_in &= (uint16_t)~script[i].keybit;
            HLE(SIMULIERT, "Tasten: Eingabe-Automatik (BIS_INPUT)");
        }
    }
}

static void vblank(CPU *c) {
    sched_trace(TR_TICK, (uint32_t)frames, 0);
    frames++;
    {   /* Fehlersuche: BIS_PROFILE_FROM=frame zählt "Häufigste" erst ab diesem Frame */
        static long long prof_from = -2;
        if (prof_from == -2) prof_from = getenv("BIS_PROFILE_FROM") ? atoll(getenv("BIS_PROFILE_FROM")) : -1;
        if (prof_from >= 0 && (long long)frames == prof_from)
            for (unsigned i = 0; i < nfns; i++) fns[i].calls = 0;
    }
    platform_inhibit_irq++;
    gpu3d_vblank();     /* 3D: Liste des vorigen Frames zeichnen, neue übernehmen */
    dma_vblank();
    platform_inhibit_irq--;
#ifndef PLATFORM_SDL
    script_apply();
#endif
#ifdef PLATFORM_SDL
    {
        /* Echtzeit: Bild zeigen, Tastatur lesen, auf 60 Bilder/s bremsen */
        static uint32_t top[256 * 192], bottom[256 * 192];
        Gfx2D g = { io, pal, oam, gpu3d_frame() };
        unsigned missing = 0;
        int a_on_top = (io16(0x304) >> 15) & 1;
        platform_inhibit_irq++;
        render2d_engine(&g, a_on_top ? 0 : 1, top, 1, &missing);
        render2d_engine(&g, a_on_top ? 1 : 0, bottom, 1, &missing);
        frontend_present(top, bottom);
        if (!frontend_poll(&keys_in, &keys_xy)) { platform_inhibit_irq--; stop("Fenster geschlossen"); }
        frontend_pace();
        platform_inhibit_irq--;
    }
#endif
    /* ARM7-Ersatz: X/Y-Tasten in den gemeinsamen Puffer (Bits 0-9 und 12-15 bleiben 0:
     * kein Debug-Knopf, Stift nicht unten, Klappe offen) */
    {
        uint8_t *xy = ptr(HW_BUTTON_XY_BUF);
        xy[0] = (uint8_t)keys_xy; xy[1] = (uint8_t)(keys_xy >> 8);
        /* ARM7-Ersatz: VBlank-Zähler im Systembereich (0x027FFC3C). Die Frame-Funktion
         * 0x01ff8d18 ruft daraus pro vergangenem Frame die Sound-Verwaltung (NNS) auf. */
        uint8_t *vc = ptr(0x027ffc3cu);
        uint32_t n = (vc[0] | vc[1] << 8 | vc[2] << 16 | (uint32_t)vc[3] << 24) + 1;
        vc[0] = (uint8_t)n; vc[1] = (uint8_t)(n >> 8); vc[2] = (uint8_t)(n >> 16); vc[3] = (uint8_t)(n >> 24);
        HLE(ORIGINAL, "ARM7: VBlank-Zaehler im Systembereich (0x027FFC3C)");
    }
    if (dump_every && frames % dump_every == 0) {
        char tag[32];
        snprintf(tag, sizeof tag, "%05llu", (unsigned long long)frames);
        platform_inhibit_irq++;
        dump_frame(tag);
        platform_inhibit_irq--;
    }
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
static uint64_t halts_locked;

void rc_mcr(CPU *c, int a, int b, int d, int e, uint32_t v) {
    (void)v;
    if (a == 0 && b == 7 && d == 0 && e == 4) {
        /* CP15 c7,c0,4 = "Warte auf Interrupt" (OS_Halt). Zeit vergeht bis zum
         * nächsten Interrupt; danach am sicheren Punkt neu planen. */
        HLE(ORIGINAL, "OS_Halt: warten bis zum naechsten Interrupt");
        if (c->cpsr_ctrl & 0x80) {
            /* Mit gesperrten Interrupts kommt nie wieder einer an (OS_Terminate) */
            if (++halts_locked > 3) stop("OS_Terminate: Prozessor mit gesperrten Interrupts angehalten");
        } else {
            halts_locked = 0;
        }
        /* Läuft eine Karten-Übertragung, ist sie lange vor dem nächsten VBlank fertig
         * (~0,1 ms je Block) und ihr Interrupt weckt den Prozessor. Ohne das zählte der
         * Countdown im Halt nicht weiter und jeder Block kostete einen Frame. */
        if (card_countdown || card_fire_pending) { card_countdown = 0; card_fire(); }
        /* Steht schon ein Interrupt an (z. B. ARM7-Antwort), weckt der den
         * Prozessor sofort; sonst vergeht ein Frame. */
        if (!deliver_pending(c)) vblank(c);
        resched_if_pending(c);
        return;
    }
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

    printf("Zuletzt ausserhalb von Interrupts: %s, gelesen:", fn_name(last_outside_irq));
    for (unsigned i = 0; i < 16; i++) printf(" %08x", rd_ring[(rd_pos + i) & 15]);
    printf("\n");

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

    /* Spiel-Heap 0 (Tabelle 0x0205b7f0): Blockkette ab dem ersten Block.
     * Block: +0 vorheriger, +4 nächster, +8 Größe (Bits 0-24) | frei (Bit 25) | Kennung (Bits 26-31) */
    printf("\nSpiel-Heap 0 (erste Bloecke):\n");
    {
        uint32_t b = rd32(0x0205b7f0u);
        for (int i = 0; i < 24 && b; i++) {
            uint32_t w = rd32(b + 8);
            printf("  %08x  vor %08x  nach %08x  groesse %07x  %s  kennung %u\n", b, rd32(b), rd32(b + 4),
                   w & 0x1ffffff, (w >> 25) & 1 ? "frei " : "belegt", w >> 26);
            b = rd32(b + 4);
        }
    }

    /* Fehlersuche: BIS_DUMP="adresse:laenge" (hex) zeigt Speicher am Ende */
    for (const char *s = getenv("BIS_DUMP"); s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL) {   /* auch "a:n,b:m" */
        char *colon;
        uint32_t a = (uint32_t)strtoul(s, &colon, 16);
        uint32_t n = *colon == ':' ? (uint32_t)strtoul(colon + 1, NULL, 16) : 0x40;
        printf("\nSpeicher ab %08x:\n", a);
        for (uint32_t i = 0; i < n; i += 16) {
            char asc[17] = {0};
            printf("  %08x ", a + i);
            for (uint32_t j = 0; j < 16; j++) {
                uint8_t b = rd8(a + i + j);
                printf(" %02x", b);
                asc[j] = b >= 0x20 && b < 0x7f ? (char)b : '.';
            }
            printf("  %s\n", asc);
        }
    }

    /* Anzeige: Wie nah sind wir an einem Bild? */
    printf("\nAnzeige-Register:\n");
    printf("  DISPCNT A %08x  B %08x   POWCNT1 %04x   VRAMCNT A-D %02x %02x %02x %02x  E-I %02x %02x %02x %02x %02x\n",
           io32(0x000), io32(0x1000), io16(0x304), io[0x240], io[0x241], io[0x242], io[0x243],
           io[0x244], io[0x245], io[0x246], io[0x248], io[0x249]);
    {
        unsigned nz_pal = 0, nz_oam = 0;
        for (unsigned i = 0; i < sizeof pal; i++) nz_pal += pal[i] != 0;
        for (unsigned i = 0; i < sizeof oam; i++) nz_oam += oam[i] != 0;
        printf("  Nicht-Null-Bytes: Paletten %u / %u, OAM %u\n", nz_pal, (unsigned)sizeof pal, nz_oam);
        {
            unsigned gp, gv; uint64_t gc;
            gpu3d_stats(&gp, &gv, &gc);
            if (getenv("BIS_3D")) gpu3d_debug();
            printf("  3D: letztes Bild %u Polygone, %u Vertices; %llu GX-Befehle gesamt; DISP3DCNT %04x CLEAR %08x/%04x\n",
                   gp, gv, (unsigned long long)gc, io16(0x60), io32(0x350), io16(0x354));
        }
        for (int e = 0; e < 2; e++) {
            uint32_t b = e ? 0x1000 : 0;
            printf("  Engine %c: BGCNT %04x %04x %04x %04x  BLDCNT %04x BLDALPHA %04x BLDY %04x"
                   "  WININ %04x WINOUT %04x  Hintergrundfarbe %04x\n", e ? 'B' : 'A',
                   io16(b + 8), io16(b + 10), io16(b + 12), io16(b + 14), io16(b + 0x50), io16(b + 0x52),
                   io16(b + 0x54), io16(b + 0x48), io16(b + 0x4a), pal[e ? 0x400 : 0] | pal[(e ? 0x400 : 0) + 1] << 8);
        }
    }

    printf("\nIn Warteschleifen unterbrochen (zuletzt betretene Funktion):\n");
    for (int i = 0; i < 16 && poll_where[i].addr; i++)
        printf("  %8llu  0x%08x  %s\n", (unsigned long long)poll_where[i].n, poll_where[i].addr, fn_name(poll_where[i].addr));

    printf("\nThreads:\n");
    printf("  Scheduler-Verstoesse: %u\n", sched_violations);
    printf("  Trace: %llu Ereignisse, Pruefsumme %016llx\n",
           (unsigned long long)sched_trace_count(), (unsigned long long)sched_trace_hash());
    printf("  Letzte Thread-Ereignisse (ohne TICK/IRQ):\n");
    sched_trace_dump_filtered(stdout, 25);
}

/* ================================================================ Start */

static void boot_entry(CPU *c, void *arg) {
    (void)arg;
    c->r[13] = 0x027e3c00u;               /* System-Stack im DTCM */
    c->cpsr_ctrl = 0x1f;                  /* System-Modus, Interrupts erlaubt */
    sched_active = 1;
    /* Reihenfolge wie am Ende von Entry */
    rc_func_02045cb8(c);
    rc_func_02019a18(c);
    rc_func_020471dc(c);
    rc_main(c);
    stop_reason = "main ist zurueckgekehrt";
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: %s <extract/eur> [frames] [budget_mio]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    frame_limit = argc > 2 ? strtoull(argv[2], NULL, 0) : 600;
    mem_budget = (argc > 3 ? strtoull(argv[3], NULL, 0) : 2000) * 1000000ull;
    ctx_addr = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 16) : 0;
    if (argc > 4) {   /* "a,b,c": mehrere Funktionen verfolgen */
        const char *s = strchr(argv[4], ',');
        for (int k = 0; s && k < 8; k++, s = strchr(s + 1, ',')) ctx_more[k] = (uint32_t)strtoul(s + 1, NULL, 16);
    }
    if (argc > 5) {   /* Watchpoint "adresse:laenge" (hex) */
        char *colon;
        watch_lo = (uint32_t)strtoul(argv[5], &colon, 16);
        watch_hi = watch_lo + (*colon == ':' ? (uint32_t)strtoul(colon + 1, NULL, 16) : 4);
    }
    if (getenv("BIS_CARD_DELAY")) card_delay = (uint32_t)strtoul(getenv("BIS_CARD_DELAY"), NULL, 0);
    if (getenv("BIS_DUMP_EVERY")) dump_every = strtoull(getenv("BIS_DUMP_EVERY"), NULL, 0);   /* Bild alle N Frames */
#ifdef PLATFORM_SDL
    /* Fenster-Modus: ohne Frame-Limit und Budget, keine PNG-Ausgabe */
    if (argc <= 2) frame_limit = 0;
    if (argc <= 3) mem_budget = UINT64_MAX;
    dump_every = 0;
    if (!frontend_init()) return 3;
#endif

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
    gpu3d_init(io);

    /* Was die Firmware vor dem Spielstart macht: ROM-Header nach 0x027FFE00,
     * Chip-ID der Karte, Boot-Typ "normales Modul" */
    char rom_path[1024];
    snprintf(rom_path, sizeof rom_path, "%s/../../orig/baserom.nds", dir);
    if (!card_load_rom(rom_path)) return 2;
    for (uint32_t i = 0; i < 0x170; i++) wr8(0x027ffe00u + i, card_rom()[i]);
    wr32(0x027ff800u, card_chip_id());
    wr32(0x027ffc00u, card_chip_id());
    wr16(0x027ffc40u, 1);
    HLE(ORIGINAL, "Firmware: ROM-Header, Chip-ID und Boot-Typ im Systembereich");
    /* WLAN-MAC-Adresse der Konsole (0x027FFCF4). Jedes DS hat eine eigene; der
     * Kopierschutz (DSProtect, ov138) wertet 00:00:00:00:00:00 und eine bekannte
     * Emulator-Adresse als Kopie und schaltet dann die Tasten in der Dateiauswahl ab. */
    {
        static const uint8_t mac[6] = { 0x00, 0x09, 0xbf, 0x5a, 0x3c, 0x71 };   /* Nintendo-OUI, frei gewählt */
        for (int i = 0; i < 6; i++) wr8(0x027ffcf4u + (uint32_t)i, mac[i]);
        HLE(SIMULIERT, "Firmware: WLAN-MAC-Adresse (feste, frei gewaehlte Adresse)");
    }

    /* Was Entry an Hardware-Einstellungen macht */
    wr32(IRQ_HANDLER_PTR, 0x01ff8d70u);   /* OS-Interrupt-Handler im ITCM */
    wr32(0x027fff9cu, 0);
    /* ARM7-Ersatz: alle PXI-Kanäle des ARM7 gelten als bereit
     * (PXI_IsCallbackReady prüft 0x027ffc00 + 0x388 + proc*4) */
    wr32(0x027ffc00u + 0x388 + 4, 0xffffffffu);
    HLE(SIMULIERT, "ARM7: alle PXI-Kanaele als bereit gemeldet (ohne echte Gegenstelle)");
    set_io16(0x130, 0x03ff);
    wr16(HW_BUTTON_XY_BUF, keys_xy);   /* X/Y nicht gedrückt (aktiv-niedrig) */
    script_load();

    printf("Funktionen: %u, Frame-Limit %llu, Budget %llu Mio. Speicherzugriffe\n",
           nfns, (unsigned long long)frame_limit, (unsigned long long)(mem_budget / 1000000));
    /* Der Start-Thread läuft als erster Fiber; er bindet sich beim ersten
     * OS_SaveContext an seine SDK-Thread-Struktur. */
    snprintf(frames_dir, sizeof frames_dir, "%s/../../build/frames", dir);
    sched_run(&cpu, BOOT_THREAD_KEY, boot_entry, NULL);
#ifndef PLATFORM_SDL
    dump_frame("ende");
#endif
    report();
#ifdef PLATFORM_SDL
    /* Nur beim echten Spielen (kein Frame-Limit) und nur bei einem Fehler offen lassen;
     * Testläufe mit Frame-Limit sollen sich selbst beenden. */
    if (!frame_limit && strcmp(stop_reason, "Fenster geschlossen") != 0) frontend_wait_close(stop_reason);
#endif
    return 0;
}
