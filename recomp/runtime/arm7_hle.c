/* ARM7-Ersatz: beantwortet PXI-Nachrichten des ARM9.
 *
 * Auf dem DS läuft auf dem ARM7 ein eigenes Programm, das Karte, Sound, Touch,
 * Mikrofon, Uhr und Power verwaltet. Der ARM9 schickt ihm über die IPC-FIFO
 * 32-Bit-Wörter im Format des NitroSDK:
 *     Bits 0-4  Kanal (PXI-Tag)
 *     Bit  5    Flag (beim Kartenprotokoll: "Befehl"/"Antwort")
 *     Bits 6-31 Daten
 * Antworten landen in der Empfangs-FIFO (0x04100000) und lösen beim ARM9
 * Interrupt-Bit 18 aus. Der PXI-Interrupt-Handler des Spiels verteilt sie dann
 * an die registrierten Callbacks.
 *
 * Wir führen keinen ARM7-Code aus, sondern antworten so, wie der ARM7 es täte.
 * Jede Antwort ist im HLE-Bericht als SIMULIERT oder FEHLT gekennzeichnet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "arm7_hle.h"

#define TAG_FS 11

/* ---------------------------------------------------------------- Empfangs-FIFO */

#define RECV_MAX 64
static uint32_t recv_q[RECV_MAX];
static unsigned recv_head, recv_tail;

static void reply(uint32_t tag, uint32_t flag, uint32_t data) {
    if (recv_tail - recv_head >= RECV_MAX) { HLE(FEHLT, "ARM7: Empfangs-FIFO voll, Antwort verworfen"); return; }
    recv_q[recv_tail++ % RECV_MAX] = (tag & 0x1f) | (flag & 1) << 5 | data << 6;
}

int arm7_recv_empty(void) { return recv_head == recv_tail; }

uint32_t arm7_recv_pop(void) {
    if (arm7_recv_empty()) return 0;
    return recv_q[recv_head++ % RECV_MAX];
}

/* ---------------------------------------------------------------- Kanal 11: Karte */

/* Befehlsnummern von CARDi_Request (NitroSDK). Nur INIT ist bisher bestätigt
 * (erste Nachricht beim Start); die übrigen Nummern folgen der SDK-Reihenfolge
 * und werden beim ersten Auftreten geprüft. */
enum {
    CARD_REQ_INIT = 0, CARD_REQ_ACK, CARD_REQ_IDENTIFY, CARD_REQ_READ_ID, CARD_REQ_READ_ROM,
    CARD_REQ_WRITE_ROM, CARD_REQ_READ_BACKUP, CARD_REQ_WRITE_BACKUP,
    CARD_REQ_PROGRAM_BACKUP, CARD_REQ_VERIFY_BACKUP,
};

static uint32_t card_cmd_buf;     /* Adresse des gemeinsamen Befehlspuffers */
static int card_expect_ptr;       /* nach INIT kommt als nächstes die Pufferadresse */

/* Befehlspuffer (CARDiCommandArg), Layout aus den beobachteten Zugriffen:
 *   +0x00 result   +0x04 type (0x0d01)   +0x08 id
 *   +0x0c src      +0x10 dest            +0x14 len
 *   +0x18 Gesamtgröße (0x2000)  +0x1c Seitengröße (0x20) ...
 * Beim Lesen ist src eine Spielstand-Adresse und dest eine RAM-Adresse,
 * beim Schreiben umgekehrt. */
#define CMD_SRC  0x0c
#define CMD_DEST 0x10
#define CMD_LEN  0x14
#define CMD_SIZE 0x18

/* Spielstandspeicher: leerer EEPROM-Baustein liefert 0xFF */
#define BACKUP_MAX 0x80000
static uint8_t backup[BACKUP_MAX];
static uint32_t backup_size = 0x2000;
static int backup_ready;
static const char *backup_path;
static int backup_dirty;

void arm7_set_save_path(const char *path) { backup_path = path; }

static void backup_init(void) {
    if (backup_ready) return;
    backup_ready = 1;
    memset(backup, 0xff, sizeof backup);
    if (backup_path) {
        FILE *f = fopen(backup_path, "rb");
        if (f) {
            size_t n = fread(backup, 1, sizeof backup, f);
            fclose(f);
            fprintf(stderr, "  [arm7] Spielstand geladen: %s (%u Bytes)\n", backup_path, (unsigned)n);
        }
    }
}

void arm7_flush_save(void) {
    if (!backup_dirty || !backup_path) return;
    FILE *f = fopen(backup_path, "wb");
    if (!f) return;
    fwrite(backup, 1, backup_size, f);
    fclose(f);
    backup_dirty = 0;
}

static int backup_range_ok(uint32_t addr, uint32_t len) {
    return addr < backup_size && len <= backup_size - addr;
}

static void card_finish(uint32_t result) {
    if (card_cmd_buf) wr32(card_cmd_buf + 0, result);   /* cmd->result */
    reply(TAG_FS, 1, 0);
}

static void card_command(uint32_t data) {
    if (card_expect_ptr) {
        card_expect_ptr = 0;
        card_cmd_buf = data;
        HLE(SIMULIERT, "ARM7 Karte: INIT (Befehlspuffer uebernommen, Erfolg gemeldet)");
        fprintf(stderr, "  [arm7] Karte: Befehlspuffer bei %08x\n", data);
        card_finish(0);
        return;
    }
    backup_init();
    uint32_t src = card_cmd_buf ? rd32(card_cmd_buf + CMD_SRC) : 0;
    uint32_t dst = card_cmd_buf ? rd32(card_cmd_buf + CMD_DEST) : 0;
    uint32_t len = card_cmd_buf ? rd32(card_cmd_buf + CMD_LEN) : 0;
    switch (data) {
    case CARD_REQ_INIT:
        card_expect_ptr = 1;   /* Antwort erst nach der Pufferadresse */
        return;
    case CARD_REQ_IDENTIFY: {
        uint32_t size = card_cmd_buf ? rd32(card_cmd_buf + CMD_SIZE) : 0;
        if (size && size <= BACKUP_MAX) backup_size = size;
        HLE(SIMULIERT, "ARM7 Karte: IDENTIFY (Spielstandspeicher vorhanden)");
        card_finish(0);
        return;
    }
    case CARD_REQ_READ_BACKUP:
        if (!backup_range_ok(src, len)) { HLE(FEHLT, "ARM7 Karte: Spielstand lesen ausserhalb des Speichers"); card_finish(0); return; }
        for (uint32_t i = 0; i < len; i++) wr8(dst + i, backup[src + i]);
        HLE(SIMULIERT, "ARM7 Karte: Spielstand lesen (aus Datei bzw. leer = 0xFF)");
        card_finish(0);
        return;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:
        /* beobachtet (Befehl 8): Quelle = RAM, Ziel = Spielstand-Adresse */
        if (!backup_range_ok(dst, len)) { HLE(FEHLT, "ARM7 Karte: Spielstand schreiben ausserhalb des Speichers"); card_finish(0); return; }
        for (uint32_t i = 0; i < len; i++) backup[dst + i] = rd8(src + i);
        backup_dirty = 1;
        HLE(SIMULIERT, "ARM7 Karte: Spielstand schreiben/programmieren");
        card_finish(0);
        return;
    case CARD_REQ_VERIFY_BACKUP: {
        /* beobachtet (Befehl 9, direkt nach 8 mit gleichen Werten): vergleichen */
        if (!backup_range_ok(dst, len)) { HLE(FEHLT, "ARM7 Karte: Spielstand pruefen ausserhalb des Speichers"); card_finish(0); return; }
        int same = 1;
        for (uint32_t i = 0; i < len && same; i++) same = backup[dst + i] == rd8(src + i);
        HLE(SIMULIERT, "ARM7 Karte: Spielstand pruefen");
        card_finish(same ? 0 : 1);   /* 1 = CARD_RESULT_FAILURE (angenommen) */
        return;
    }
    default: {
        static int shown;
        HLE(FEHLT, "ARM7 Karte: Befehl ohne Implementierung (Erfolg gemeldet)");
        if (shown++ < 10) {
            fprintf(stderr, "  [arm7] Karte: Befehl %u, Puffer:", data);
            for (int i = 0; i < 12 && card_cmd_buf; i++) fprintf(stderr, " %08x", rd32(card_cmd_buf + 4 * (uint32_t)i));
            fprintf(stderr, "\n");
        }
        card_finish(0);
        return;
    }
    }
}

/* ---------------------------------------------------------------- Kanal 5: Uhr */

#define TAG_RTC 5
#define RTC_DATE 0x027ffde8u   /* BCD: Jahr | Monat<<8 | Tag<<16 | Wochentag<<24 */
#define RTC_TIME 0x027ffdecu   /* BCD: Stunde | Minute<<8 | Sekunde<<16 */

/* Feste Zeit für reproduzierbare Läufe: Fr, 09.10.2009, 12:00:00 */
#define FIXED_DATE (0x09u | 0x10u << 8 | 0x09u << 16 | 5u << 24)
#define FIXED_TIME (0x12u | 0x00u << 8 | 0x00u << 16)

static void rtc_command(uint32_t data) {
    uint32_t cmd = (data >> 8) & 0x7f;
    switch (cmd) {
    case 0x10: wr32(RTC_DATE, FIXED_DATE); wr32(RTC_TIME, FIXED_TIME);
        HLE(SIMULIERT, "ARM7 Uhr: Datum+Zeit lesen (feste Zeit 09.10.2009 12:00)"); break;
    case 0x11: wr32(RTC_DATE, FIXED_DATE);
        HLE(SIMULIERT, "ARM7 Uhr: Datum lesen (fest)"); break;
    case 0x12: wr32(RTC_TIME, FIXED_TIME);
        HLE(SIMULIERT, "ARM7 Uhr: Zeit lesen (fest)"); break;
    default: {
        static int shown;
        HLE(FEHLT, "ARM7 Uhr: Befehl ohne Implementierung (Erfolg gemeldet)");
        if (shown++ < 10) fprintf(stderr, "  [arm7] Uhr: Befehl %02x\n", cmd);
        break;
    }
    }
    reply(TAG_RTC, 0, cmd << 8);   /* Befehl zurück, Ergebnis 0 = Erfolg */
}

/* ---------------------------------------------------------------- Kanal 6: Touch */

#define TAG_TP 6
#define TP_RAW 0x027fffaau   /* HW_TOUCHPANEL_BUF: x | y<<12 | berührt<<24 | Gültigkeit<<25 */

/* Befehle kommen in Paketen (Sender z. B. 0x0203f490 = TP_RequestAutoSamplingStart):
 *   erstes Paket:  Bit 25 | Befehl<<8 | Argument (z. B. Frequenz)
 *   letztes Paket: Bit 24 | 0x10000 | Argument (z. B. VCount-Zeile)
 * Ein Befehl aus einem einzigen Paket hat Bit 24 und 25 gesetzt.
 * Antwort (TPi_TpCallback, 0x0203efdc): Bit 24 | Befehl<<8 | Ergebnis (0 = Erfolg).
 * Der Callback löscht dann das Beschäftigt-Bit (1 << Befehl). */
static uint32_t tp_cmd;

static void tp_command(uint32_t data) {
    if (data & 0x02000000u) tp_cmd = (data >> 8) & 0x7f;   /* erstes Paket: Befehl merken */
    if (!(data & 0x01000000u)) return;            /* erst nach dem letzten Paket antworten */
    uint32_t cmd = tp_cmd;
    if (cmd == 0) {
        wr32(TP_RAW, 0);                          /* Einzelmessung: nicht berührt */
        HLE(SIMULIERT, "ARM7 Touch: Messung (kein Stylus auf dem Bildschirm)");
    } else {
        HLE(SIMULIERT, "ARM7 Touch: Befehl bestaetigt (z. B. Auto-Abtastung)");
    }
    reply(TAG_TP, 0, 0x01000000u | cmd << 8);
}

/* ---------------------------------------------------------------- Kanal 7: Sound */

#define TAG_SND 7
#define SND_SHARED_WORK_PTR 0x020624e0u   /* ARM9-Variable: Zeiger auf SNDSharedWork */

/* Der ARM9 schickt pro "Flush" die Adresse einer Befehlsliste. Der echte ARM7
 * arbeitet sie ab und erhöht danach SNDSharedWork.finishCommandTag (+0). Darauf
 * wartet der ARM9 (0x0203b9a8 liest den Zähler, dazwischen OS_SpinWait).
 * Wir führen die Befehle (noch) nicht aus -> kein Ton, aber kein Warten. */
static void snd_command(uint32_t data) {
    if (data == 0) {
        /* 0x0203b7cc: "jetzt verarbeiten"-Signal ohne neue Liste – zählt nicht */
        HLE(SIMULIERT, "ARM7 Sound: Verarbeiten-Signal (ohne neue Liste)");
        return;
    }
    uint32_t sw = rd32(SND_SHARED_WORK_PTR);
    if (getenv("BIS_SNDLOG")) {
        /* Fehlersuche: Befehlsliste ausgeben. SDK-Layout: next, id, arg[4] (0x18 Bytes) */
        static int shown;
        int n = 0;
        for (uint32_t c = data; c && n < 64 && shown < 3000; c = rd32(c), n++) {
            uint32_t id = rd32(c + 4);
            if (id == 20 || id == 21 || id == 33) continue;   /* Lautstärke/Pan/Treiberinfo: ständig */
            shown++;
            fprintf(stderr, "  [snd] Befehl %2u  %08x %08x %08x %08x\n", id, rd32(c + 8), rd32(c + 12), rd32(c + 16), rd32(c + 20));
        }
    }
    if (sw && getenv("BIS_SND_STATUS")) {
        /* EXPERIMENT: Spielerstatus pflegen (SNDSharedWork.playerStatus, +4).
         * Befehle: 0 START_SEQ, 1 STOP_SEQ, 2 PREPARE_SEQ, 3 START_PREPARED_SEQ; arg0 = Spieler */
        int n = 0;
        for (uint32_t c = data; c && n < 256; c = rd32(c), n++) {
            uint32_t id = rd32(c + 4), p = rd32(c + 8) & 15;
            uint32_t st = rd32(sw + 4);
            if (id == 0 || id == 2 || id == 3) st |= 1u << p;
            else if (id == 1) st &= ~(1u << p);
            else continue;
            wr32(sw + 4, st);
            HLE(SIMULIERT, "ARM7 Sound: Spielerstatus aus Start/Stopp-Befehlen (ohne Wiedergabe)");
        }
    }
    if (sw) {
        wr32(sw, rd32(sw) + 1);
        HLE(SIMULIERT, "ARM7 Sound: Befehlsliste als erledigt gemeldet (kein Ton)");
    } else {
        HLE(FEHLT, "ARM7 Sound: Befehlsliste vor Einrichtung des gemeinsamen Speichers");
    }
}

/* ---------------------------------------------------------------- Eingang */

void arm7_pxi_send(uint32_t word) {
    uint32_t tag = word & 0x1f, flag = (word >> 5) & 1, data = word >> 6;
    (void)flag;
    platform_inhibit_irq++;   /* unser Ersatz-Code darf nicht unterbrochen werden */
    switch (tag) {
    case TAG_FS:
        card_command(data);
        break;
    case TAG_RTC:
        rtc_command(data);
        break;
    case TAG_TP:
        tp_command(data);
        break;
    case TAG_SND:
        snd_command(data);
        break;
    default: {
        static int shown;
        HLE(FEHLT, "ARM7: Nachricht auf einem Kanal ohne Ersatz (verworfen)");
        if (shown++ < 10)
            fprintf(stderr, "  [arm7] Kanal %u ohne Ersatz: Daten %07x Flag %u\n", tag, data, flag);
        break;
    }
    }
    platform_inhibit_irq--;
}
