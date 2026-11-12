/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * emulator_modem: a simulated LTE modem with a SIM card for the emulator.
 *
 * It answers the AT dialect of the stock RIL vendor library
 * (base/telephony/ril_adapter/services/vendor, libril_vendor.z.so) on a
 * pseudo-terminal linked as /dev/ttyUSB0, the library's default AT port.
 * The phone then registers on the "Oniro" LTE network: SIM present, signal
 * bars, outgoing calls (no audio), and SMS both ways.
 *
 * The host drives it with parameters over hdc:
 *   param set emulator.modem.signal 2            signal bars, 0-4
 *   param set emulator.modem.sms "+447700900123 Hello"   deliver an SMS
 *   param set emulator.modem.call +447700900123  incoming call
 *   param set emulator.modem.call hangup         the remote side hangs up
 * The sms and call parameters are consumed: the modem clears them.
 *
 * Mobile data is not offered (the vendor library brings up a "usb0"
 * interface the emulator does not have); the ethernet link carries data.
 *
 * The SIM file contents and most replies follow harmony-contrib/ohos-qemu's
 * qemu_at_modem.py, written against the same vendor library.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "hilog/log.h"
#include "parameter.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD001F00
#define LOG_TAG "EmulatorModem"
#define LOGI(...) HILOG_INFO(LOG_CORE, __VA_ARGS__)
#define LOGW(...) HILOG_WARN(LOG_CORE, __VA_ARGS__)
#define LOGE(...) HILOG_ERROR(LOG_CORE, __VA_ARGS__)

#define TTY_LINK "/dev/ttyUSB0"
#define PARAM_SIGNAL "emulator.modem.signal"
#define PARAM_SMS "emulator.modem.sms"
#define PARAM_CALL "emulator.modem.call"
#define PARAM_LEN 512
#define LINE_MAX_LEN 4096
#define OUT_MAX_LEN 8192
#define MAX_CALLS 7
#define MAX_TIMERS 16
#define PARAM_POLL_MS 500
#define RADIO_GID 1001

#define IMSI "001010123456789"
#define IMEI "358240051111110"
#define OPERATOR_LONG "Oniro"
#define OPERATOR_SHORT "Oniro"
#define OPERATOR_NUMERIC "00101"
#define OWN_NUMBER "+15555215554"
#define LAC "0001"
#define CELL_ID "00000101"
#define ACT_LTE 7

/* --- SIM elementary files (GSM 11.11 / TS 31.102), answered to AT+CRSM --- */

struct SimFile {
    int id;
    int linear;     /* 0 transparent, 1 linear fixed */
    int recordLen;
    const char *hex;
};

static const struct SimFile SIM_FILES[] = {
    { 0x2FE2, 0, 0, "98101010325476981000" },                 /* ICCID 8901010123456789010 */
    { 0x6F07, 0, 0, "080910101032547698" },                   /* IMSI 001010123456789 */
    { 0x6FAD, 0, 0, "00000002" },                             /* AD: MNC length 2 */
    { 0x6FAE, 0, 0, "03" },                                   /* phase 2+ */
    { 0x6F46, 0, 0, "014F6E69726FFFFFFFFFFFFFFFFFFFFFFF" },   /* SPN "Oniro" */
    { 0x6F38, 0, 0, "FF3FFF0300003F03000C0000F00300" },       /* SST */
    { 0x6F78, 0, 0, "0001" },                                 /* ACC */
    { 0x6F7E, 0, 0, "FFFFFFFF00F1100001FF00" },               /* LOCI */
    { 0x6F7B, 0, 0, "FFFFFFFFFFFFFFFFFFFFFFFF" },             /* FPLMN */
    { 0x6F3E, 0, 0, "FFFF" },                                 /* GID1 */
    { 0x6F3F, 0, 0, "FFFF" },                                 /* GID2 */
    { 0x6F05, 0, 0, "656EFFFFFFFF" },                         /* language "en" */
    { 0x6F16, 0, 0, "000000" },                               /* CPHS info */
    { 0x6F15, 0, 0, "0000" },                                 /* CSP */
    { 0x6F14, 0, 0, "4F6E69726FFFFFFFFFFFFFFFFFFFFF" },       /* ONS "Oniro" */
    { 0x6F18, 0, 0, "4F6E69726FFFFFFFFFFF" },                 /* ONS short */
    /* MSISDN +15555215554: 14 bytes alpha, then length, TON, BCD digits */
    { 0x6F40, 1, 28, "FFFFFFFFFFFFFFFFFFFFFFFFFFFF07915155255155F4FFFFFFFFFFFF" },
    /* SMSP: only the service centre, +15555000000 */
    { 0x6F42, 1, 28, "FDFFFFFFFFFFFFFFFFFFFFFFFF07915155050000F0FFFFFFFF0000FF" },
    { 0x6FCB, 1, 16, "0100FFFFFFFFFFFFFFFFFFFFFFFFFFFF" },   /* CFIS */
    { 0x6FCA, 1, 5, "0000000000" },                           /* MWIS */
    { 0x6FC7, 1, 28, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" }, /* MBDN */
    { 0x6FC9, 1, 4, "01010101" },                             /* MBI */
};

static const struct SimFile *FindSimFile(int id)
{
    for (size_t i = 0; i < sizeof(SIM_FILES) / sizeof(SIM_FILES[0]); i++) {
        if (SIM_FILES[i].id == id) {
            return &SIM_FILES[i];
        }
    }
    return NULL;
}

/* --- state --- */

struct Call {
    int used;
    int id;
    int dir;    /* 0 outgoing, 1 incoming */
    int state;  /* 0 active, 1 held, 2 dialing, 3 alerting, 4 incoming, 5 waiting */
    int mpty;
    char number[64];
};

struct Timer {
    int used;
    int64_t dueMs;
    int callId;
    int newState;   /* -1: release the call */
};

static int g_master = -1;
static int g_radioOn = 0;
static int g_signalBars = 4;
static int g_smsMode = 0;           /* waiting for a PDU after AT+CMGS / AT+CMGW */
static int g_smsStoring = 0;        /* AT+CMGW rather than AT+CMGS */
static int g_messageRef = 0;
static struct Call g_calls[MAX_CALLS];
static struct Timer g_timers[MAX_TIMERS];
static char g_out[OUT_MAX_LEN];
static size_t g_outLen = 0;

static int64_t NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- output: replies are queued and flushed after each command --- */

static void Out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + g_outLen, sizeof(g_out) - g_outLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_outLen += (size_t)n < sizeof(g_out) - g_outLen ? (size_t)n : sizeof(g_out) - g_outLen - 1;
    }
}

static void Flush(void)
{
    size_t off = 0;
    while (off < g_outLen) {
        ssize_t n = write(g_master, g_out + off, g_outLen - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            LOGE("write to the AT port failed: %{public}d", errno);
            break;
        }
        off += (size_t)n;
    }
    g_outLen = 0;
}

/* Unsolicited result: a line of its own, sent at once. */
static void Unsolicited(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Out("\r\n%s\r\n", line);
    Flush();
}

/* --- signal --- */

static void SignalValues(int *rxlev, int *rsrq, int *rsrp, int *snr)
{
    /* RSRP in -dBm per bar count, inside core_service's 4-bar thresholds
     * (-121 -115 -109 -103 -97, signal_information.h). */
    static const int RSRP[] = { 120, 112, 106, 100, 90 };
    int bars = g_signalBars < 0 ? 0 : (g_signalBars > 4 ? 4 : g_signalBars);
    *rsrp = RSRP[bars];
    *rxlev = *rsrp - 30;
    *rsrq = 10;
    *snr = bars * 5;
}

static void ReportSignal(void)
{
    int rxlev, rsrq, rsrp, snr;
    SignalValues(&rxlev, &rsrq, &rsrp, &snr);
    Unsolicited("^HCSQ: \"LTE\",%d,%d,%d,%d", rxlev, rsrq, rsrp, snr);
}

/* --- calls --- */

static struct Call *FindCall(int id)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (g_calls[i].used && g_calls[i].id == id) {
            return &g_calls[i];
        }
    }
    return NULL;
}

static struct Call *NewCall(int dir, int state, const char *number)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (!g_calls[i].used) {
            struct Call *c = &g_calls[i];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->id = i + 1;
            c->dir = dir;
            c->state = state;
            snprintf(c->number, sizeof(c->number), "%s", number);
            return c;
        }
    }
    return NULL;
}

/* ^CCALLSTATE makes the vendor library re-read the call list (AT+CLCC). */
static void CallStateChanged(int id, int state)
{
    Unsolicited("^CCALLSTATE: %d,%d,0", id, state);
}

static void ReleaseCall(struct Call *c)
{
    int id = c->id;
    c->used = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].used && g_timers[i].callId == id) {
            g_timers[i].used = 0;
        }
    }
    CallStateChanged(id, 6);
}

static void ReleaseWhere(int (*match)(const struct Call *))
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (g_calls[i].used && match(&g_calls[i])) {
            ReleaseCall(&g_calls[i]);
        }
    }
}

static int IsAny(const struct Call *c) { (void)c; return 1; }
static int IsActive(const struct Call *c) { return c->state == 0; }
static int IsHeldOrWaiting(const struct Call *c) { return c->state == 1 || c->state == 5; }

static void AddTimer(int64_t delayMs, int callId, int newState)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].used) {
            g_timers[i] = (struct Timer){ 1, NowMs() + delayMs, callId, newState };
            return;
        }
    }
}

static void RunTimers(void)
{
    int64_t now = NowMs();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].used || g_timers[i].dueMs > now) {
            continue;
        }
        g_timers[i].used = 0;
        struct Call *c = FindCall(g_timers[i].callId);
        if (c == NULL) {
            continue;
        }
        if (g_timers[i].newState < 0) {
            ReleaseCall(c);
        } else {
            c->state = g_timers[i].newState;
            CallStateChanged(c->id, c->state);
        }
    }
}

static int NextTimerMs(void)
{
    int64_t now = NowMs();
    int64_t best = PARAM_POLL_MS;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].used) {
            int64_t d = g_timers[i].dueMs - now;
            if (d < best) {
                best = d < 0 ? 0 : d;
            }
        }
    }
    return (int)best;
}

static void ListCalls(void)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        struct Call *c = &g_calls[i];
        if (c->used) {
            Out("+CLCC: %d,%d,%d,0,%d,\"%s\",%d\r\n", c->id, c->dir, c->state, c->mpty, c->number,
                c->number[0] == '+' ? 145 : 129);
        }
    }
}

static void Dial(const char *arg)
{
    char number[64] = {0};
    size_t n = 0;
    for (const char *p = arg; *p != '\0' && *p != ';' && n < sizeof(number) - 1; p++) {
        if (isdigit((unsigned char)*p) || *p == '+' || *p == '*' || *p == '#') {
            number[n++] = *p;
        }
    }
    /* A new call puts the active one on hold, as a real network does. */
    for (int i = 0; i < MAX_CALLS; i++) {
        if (g_calls[i].used && g_calls[i].state == 0) {
            g_calls[i].state = 1;
        }
    }
    struct Call *c = NewCall(0, 2, number);
    if (c == NULL) {
        Out("ERROR\r\n");
        return;
    }
    Out("OK\r\n");
    Flush();
    CallStateChanged(c->id, 2);
    AddTimer(1000, c->id, 3);
    AddTimer(3000, c->id, 0);
    LOGI("outgoing call %{public}d", c->id);
}

static void Answer(void)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        struct Call *c = &g_calls[i];
        if (c->used && (c->state == 4 || c->state == 5)) {
            c->state = 0;
            Out("OK\r\n");
            Flush();
            CallStateChanged(c->id, 0);
            return;
        }
    }
    Out("NO CARRIER\r\n");
}

/* AT+CHLD per TS 22.030: 0 release held/waiting, 1 release active (and
 * accept the other), 1x release call x, 2 swap, 2x split, 3 conference. */
static void Chld(const char *arg)
{
    int op = arg[0] - '0';
    int idx = isdigit((unsigned char)arg[1]) ? atoi(arg + 1) : 0;
    Out("OK\r\n");
    Flush();
    if (op == 0) {
        ReleaseWhere(IsHeldOrWaiting);
    } else if (op == 1 && idx > 0) {
        struct Call *c = FindCall(idx);
        if (c != NULL) {
            ReleaseCall(c);
        }
    } else if (op == 1 || op == 2) {
        if (op == 1) {
            ReleaseWhere(IsActive);
        }
        for (int i = 0; i < MAX_CALLS; i++) {
            struct Call *c = &g_calls[i];
            if (!c->used) {
                continue;
            }
            if (c->state == 0 && op == 2) {
                c->state = 1;
            } else if (c->state == 1 || c->state == 5) {
                c->state = 0;
            } else {
                continue;
            }
            CallStateChanged(c->id, c->state);
        }
    } else if (op == 3) {
        for (int i = 0; i < MAX_CALLS; i++) {
            if (g_calls[i].used && (g_calls[i].state == 0 || g_calls[i].state == 1)) {
                g_calls[i].state = 0;
                g_calls[i].mpty = 1;
                CallStateChanged(g_calls[i].id, 0);
            }
        }
    }
}

static void IncomingCall(const char *number)
{
    int busy = 0;
    for (int i = 0; i < MAX_CALLS; i++) {
        busy |= g_calls[i].used;
    }
    struct Call *c = NewCall(1, busy ? 5 : 4, number);
    if (c == NULL) {
        return;
    }
    LOGI("incoming call %{public}d", c->id);
    CallStateChanged(c->id, c->state);
    Unsolicited("+CRING: VOICE");
}

/* --- SMS --- */

/* Semi-octet BCD of a phone number; returns the digit count. */
static int AppendAddress(char *pdu, size_t len, const char *number)
{
    const char *digits = number[0] == '+' ? number + 1 : number;
    char clean[32] = {0};
    size_t n = 0;
    for (const char *p = digits; *p != '\0' && n < sizeof(clean) - 1; p++) {
        if (isdigit((unsigned char)*p)) {
            clean[n++] = *p;
        }
    }
    size_t used = strlen(pdu);
    used += (size_t)snprintf(pdu + used, len - used, "%02X%02X", (unsigned)n, number[0] == '+' ? 0x91 : 0x81);
    for (size_t i = 0; i < n; i += 2) {
        char lo = clean[i];
        char hi = i + 1 < n ? clean[i + 1] : 'F';
        used += (size_t)snprintf(pdu + used, len - used, "%c%c", hi, lo);
    }
    return (int)n;
}

/* UTF-8 to UTF-16BE, which phones accept as the "UCS-2" SMS alphabet
 * (characters outside the BMP as surrogate pairs); returns the byte count. */
static size_t Utf8ToUtf16(const char *in, uint8_t *out, size_t maxBytes)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p != '\0') {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1] != '\0') {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] != '\0' && p[2] != '\0') {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] != '\0' && p[2] != '\0' && p[3] != '\0') {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            cp = '?';
            p++;
            while ((*p & 0xC0) == 0x80) {
                p++;
            }
        }
        if (cp >= 0x10000) {
            if (o + 4 > maxBytes) {
                break;
            }
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 | (v >> 10);
            uint32_t lo = 0xDC00 | (v & 0x3FF);
            out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)hi;
            out[o++] = (uint8_t)(lo >> 8);
            out[o++] = (uint8_t)lo;
        } else {
            if (o + 2 > maxBytes) {
                break;
            }
            out[o++] = (uint8_t)(cp >> 8);
            out[o++] = (uint8_t)cp;
        }
    }
    return o;
}

static int Bcd(int v)
{
    return ((v % 10) << 4) | (v / 10);
}

/* SMS-DELIVER in UCS-2, delivered with +CMT (AT+CNMI=1,2,...). */
static void DeliverSms(const char *number, const char *text)
{
    char pdu[512] = "00"; /* no SMSC address: the TPDU starts after this octet */
    size_t tpduStart = strlen(pdu);
    snprintf(pdu + strlen(pdu), sizeof(pdu) - strlen(pdu), "04"); /* SMS-DELIVER */
    AppendAddress(pdu, sizeof(pdu), number);
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    snprintf(pdu + strlen(pdu), sizeof(pdu) - strlen(pdu), "0008%02X%02X%02X%02X%02X%02X00",
             Bcd(utc.tm_year % 100), Bcd(utc.tm_mon + 1), Bcd(utc.tm_mday), Bcd(utc.tm_hour),
             Bcd(utc.tm_min), Bcd(utc.tm_sec));
    uint8_t ud[140];
    size_t udl = Utf8ToUtf16(text, ud, sizeof(ud));
    snprintf(pdu + strlen(pdu), sizeof(pdu) - strlen(pdu), "%02X", (unsigned)udl);
    for (size_t i = 0; i < udl; i++) {
        snprintf(pdu + strlen(pdu), sizeof(pdu) - strlen(pdu), "%02X", ud[i]);
    }
    size_t tpduLen = (strlen(pdu) - tpduStart) / 2;
    LOGI("delivering an SMS of %{public}zu UTF-16 bytes", udl);
    Out("\r\n+CMT: ,%zu\r\n%s\r\n", tpduLen, pdu);
    Flush();
}

/* --- AT+CRSM: SIM file access --- */

static void Crsm(const char *args)
{
    int cmd = 0, fileId = 0, p1 = 0, p2 = 0, p3 = 0;
    sscanf(args, "%d,%d,%d,%d,%d", &cmd, &fileId, &p1, &p2, &p3);
    const struct SimFile *f = FindSimFile(fileId);
    char data[600] = {0};

    if (cmd == 192) { /* GET RESPONSE: GSM 11.11 15-byte header */
        int size = 16;
        if (f != NULL) {
            size = f->linear ? f->recordLen : (int)strlen(f->hex) / 2;
        }
        snprintf(data, sizeof(data), "0000%04X%04X04000000000102%02X%02X", size, fileId,
                 f != NULL ? f->linear : 0, f != NULL ? f->recordLen : 0);
    } else if (cmd == 176) { /* READ BINARY */
        const char *hex = f != NULL ? f->hex : "";
        size_t start = (size_t)((p1 << 8) | p2) * 2;
        size_t want = p3 > 0 ? (size_t)p3 * 2 : (strlen(hex) > start ? strlen(hex) - start : 0);
        for (size_t i = 0; i < want && i < sizeof(data) - 1; i++) {
            data[i] = start + i < strlen(hex) ? hex[start + i] : 'F';
        }
    } else if (cmd == 178) { /* READ RECORD: one record per file here */
        size_t want = f != NULL && f->linear ? (size_t)f->recordLen * 2 : (size_t)(p3 > 0 ? p3 : 16) * 2;
        const char *hex = f != NULL && p1 <= 1 ? f->hex : "";
        for (size_t i = 0; i < want && i < sizeof(data) - 1; i++) {
            data[i] = i < strlen(hex) ? hex[i] : 'F';
        }
    }
    /* UPDATE BINARY / RECORD and anything else: accepted, not stored. */
    Out("+CRSM: 144,0,\"%s\"\r\nOK\r\n", data);
}

/* --- command dispatch --- */

static int StartsWith(const char *s, const char *prefix)
{
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static int Is(const char *s, const char *cmd)
{
    return strcasecmp(s, cmd) == 0;
}

static void Registration(const char *name)
{
    if (g_radioOn) {
        Out("%s: 2,1,\"%s\",\"%s\",%d\r\nOK\r\n", name, LAC, CELL_ID, ACT_LTE);
    } else {
        Out("%s: 2,0\r\nOK\r\n", name);
    }
}

/* The radio state, for the host: emulator.modem.radio is "on" or "off". */
static void TraceRadio(int on)
{
    SetParameter("emulator.modem.radio", on ? "on" : "off");
}

static void RadioOn(int on)
{
    g_radioOn = on;
    Out("OK\r\n");
    Flush();
    LOGI("radio %{public}s", on ? "on" : "off");
    TraceRadio(on);
    if (on) {
        Unsolicited("^SIMST: 1");
        Unsolicited("+CREG: 1,\"%s\",\"%s\",%d", LAC, CELL_ID, ACT_LTE);
        Unsolicited("+CGREG: 1,\"%s\",\"%s\",%d", LAC, CELL_ID, ACT_LTE);
        ReportSignal();
    } else {
        ReleaseWhere(IsAny);
        Unsolicited("+CREG: 0");
        Unsolicited("+CGREG: 0");
    }
}

static void Command(char *line)
{
    if (Is(line, "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?")) {
        if (g_radioOn) {
            Out("+COPS: 0,0,\"" OPERATOR_LONG "\",7\r\n+COPS: 0,1,\"" OPERATOR_SHORT
                "\",7\r\n+COPS: 0,2,\"" OPERATOR_NUMERIC "\",7\r\nOK\r\n");
        } else {
            Out("+COPS: 0\r\n+COPS: 0\r\n+COPS: 0\r\nOK\r\n");
        }
    } else if (StartsWith(line, "AT+CFUN=")) {
        RadioOn(atoi(line + 8) == 1);
    } else if (Is(line, "AT+CFUN?")) {
        Out("+CFUN: %d\r\nOK\r\n", g_radioOn);
    } else if (Is(line, "AT^CARDMODE") || Is(line, "AT^CARDMODE?")) {
        Out("^CARDMODE: 1\r\nOK\r\n");
    } else if (Is(line, "AT+CPIN?")) {
        Out("+CPIN: READY\r\nOK\r\n");
    } else if (Is(line, "AT^CPIN?")) {
        Out("^CPIN: READY,3,10,3,10,3\r\nOK\r\n");
    } else if (Is(line, "AT^CPIN2?")) {
        Out("^CPIN2: READY,3,10,3,10,3\r\nOK\r\n");
    } else if (Is(line, "AT+CIMI")) {
        Out(IMSI "\r\nOK\r\n");
    } else if (StartsWith(line, "AT+CRSM=")) {
        Crsm(line + 8);
    } else if (StartsWith(line, "AT+CLCK=") && strstr(line, ",2") != NULL) {
        Out("+CLCK: 0\r\nOK\r\n");
    } else if (Is(line, "AT+CGSN") || Is(line, "AT+GSN")) {
        Out(IMEI "\r\nOK\r\n");
    } else if (Is(line, "AT+MEID")) {
        Out("+MEID: A0000000000000\r\nOK\r\n");
    } else if (Is(line, "AT+CGMR")) {
        Out("Oniro-Emulator-Modem-1.0\r\nOK\r\n");
    } else if (Is(line, "AT+CGMM")) {
        Out("Oniro Emulator LTE\r\nOK\r\n");
    } else if (Is(line, "AT+CGMI")) {
        Out("Oniro\r\nOK\r\n");
    } else if (Is(line, "AT+CREG?")) {
        Registration("+CREG");
    } else if (Is(line, "AT+CGREG?")) {
        Registration("+CGREG");
    } else if (Is(line, "AT+CEREG?")) {
        Registration("+CEREG");
    } else if (Is(line, "AT+COPS?")) {
        if (g_radioOn) {
            Out("+COPS: 0,0,\"" OPERATOR_LONG "\",7\r\nOK\r\n");
        } else {
            Out("+COPS: 0\r\nOK\r\n");
        }
    } else if (Is(line, "AT+COPS=?")) {
        Out("+COPS: (2,\"" OPERATOR_LONG "\",\"" OPERATOR_SHORT "\",\"" OPERATOR_NUMERIC "\",7),,(0,1,2,3,4),"
            "(0,1,2)\r\nOK\r\n");
    } else if (Is(line, "AT^HCSQ?")) {
        int rxlev, rsrq, rsrp, snr;
        SignalValues(&rxlev, &rsrq, &rsrp, &snr);
        /* ProcessParamSignalStrength skips two integers before the RAT. */
        Out("^HCSQ: 0,0,\"LTE\",%d,%d,%d,%d\r\nOK\r\n", rxlev, rsrq, rsrp, snr);
    } else if (Is(line, "AT+CSQ")) {
        Out("+CSQ: %d,99\r\nOK\r\n", 8 + g_signalBars * 5);
    } else if (Is(line, "AT^SYSINFOEX")) {
        Out("^SYSINFOEX: 2,3,0,1,,6,\"LTE\",101,\"LTE\"\r\nOK\r\n");
    } else if (Is(line, "AT^MONSC") || Is(line, "AT^MONNC")) {
        Out("^MONSC: LTE,001,01,1650,101,257,1,-90,-10,-60\r\nOK\r\n");
    } else if (Is(line, "AT+CIREG?")) {
        Out("+CIREG: 2,0\r\nOK\r\n");
    } else if (StartsWith(line, "AT^NDISDUP=") || StartsWith(line, "AT^DHCP")) {
        /* No mobile data on the emulator: see the header comment. */
        Out("+CME ERROR: 30\r\n");
    } else if (Is(line, "AT+CGACT?")) {
        Out("+CGACT: 1,0\r\nOK\r\n");
    } else if (Is(line, "AT+CLCC")) {
        ListCalls();
        Out("OK\r\n");
    } else if (StartsWith(line, "ATD")) {
        if (!g_radioOn) {
            Out("NO CARRIER\r\n");
        } else {
            Dial(line + 3);
        }
    } else if (Is(line, "ATA")) {
        Answer();
    } else if (Is(line, "ATH") || Is(line, "AT+CHUP")) {
        Out("OK\r\n");
        Flush();
        ReleaseWhere(IsAny);
    } else if (StartsWith(line, "AT+CHLD=")) {
        Chld(line + 8);
    } else if (Is(line, "AT+CSCA?")) {
        Out("+CSCA: \"+15555000000\",145\r\nOK\r\n");
    } else if (Is(line, "AT+CNUM")) {
        Out("+CNUM: ,\"" OWN_NUMBER "\",145\r\nOK\r\n");
    } else if (StartsWith(line, "AT+CMGS=") || StartsWith(line, "AT+CMGW=")) {
        g_smsMode = 1;
        g_smsStoring = StartsWith(line, "AT+CMGW=");
        /* "> " then a line end: the vendor library's reader has a special
         * case for a read that ends exactly in "> " which steps past the
         * data and glues stale buffer bytes onto the next reply. */
        Out("\r\n> \r\n");
    } else if (Is(line, "AT+CPMS?")) {
        Out("+CPMS: \"SM\",0,50,\"SM\",0,50,\"ME\",0,100\r\nOK\r\n");
    } else if (StartsWith(line, "AT+CPMS=")) {
        Out("+CPMS: 0,50,0,50,0,100\r\nOK\r\n");
    } else {
        /* Configuration commands (unsolicited-report switches, CNMI, CMEE...),
         * DTMF, mute, supplementary services: accepted. */
        Out("OK\r\n");
    }
}

static void SmsPdu(const char *pdu)
{
    g_smsMode = 0;
    g_messageRef = (g_messageRef + 1) & 0xFF;
    LOGI("%{public}s an SMS (%{public}zu PDU bytes)", g_smsStoring ? "stored" : "sent", strlen(pdu) / 2);
    Out("%s: %d\r\nOK\r\n", g_smsStoring ? "+CMGW" : "+CMGS", g_smsStoring ? 1 : g_messageRef);
}

/* --- host parameters --- */

static void PollParams(void)
{
    char value[PARAM_LEN];

    if (GetParameter(PARAM_SIGNAL, "4", value, sizeof(value)) > 0) {
        int bars = atoi(value);
        if (bars != g_signalBars) {
            g_signalBars = bars;
            LOGI("signal %{public}d bars", bars);
            if (g_radioOn) {
                ReportSignal();
            }
        }
    }

    if (GetParameter(PARAM_SMS, "", value, sizeof(value)) > 0 && value[0] != '\0') {
        SetParameter(PARAM_SMS, "");
        char *text = strchr(value, ' ');
        if (text != NULL) {
            *text++ = '\0';
        }
        if (g_radioOn) {
            DeliverSms(value, text != NULL ? text : "");
        } else {
            LOGW("radio off: SMS not delivered");
        }
    }

    if (GetParameter(PARAM_CALL, "", value, sizeof(value)) > 0 && value[0] != '\0') {
        SetParameter(PARAM_CALL, "");
        if (strcasecmp(value, "hangup") == 0) {
            ReleaseWhere(IsAny);
        } else if (g_radioOn) {
            IncomingCall(value);
        } else {
            LOGW("radio off: no incoming call");
        }
    }
}

/* --- the port --- */

static int OpenPort(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        LOGE("cannot create a pseudo-terminal: %{public}d", errno);
        return -1;
    }
    const char *slave = ptsname(master);
    if (slave == NULL) {
        return -1;
    }
    /* Keep the slave open ourselves: its raw settings survive the vendor
     * library closing and reopening it, and reads on the master never see
     * EIO while nobody has the port open. */
    int keep = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (keep < 0) {
        return -1;
    }
    struct termios ios;
    tcgetattr(keep, &ios);
    cfmakeraw(&ios);
    tcsetattr(keep, TCSANOW, &ios);
    if (chown(slave, 0, RADIO_GID) != 0 || chmod(slave, 0660) != 0) {
        LOGW("cannot set the owner of %{public}s: %{public}d", slave, errno);
    }
    unlink(TTY_LINK);
    if (symlink(slave, TTY_LINK) != 0) {
        LOGE("cannot link %{public}s to %{public}s: %{public}d", TTY_LINK, slave, errno);
        return -1;
    }
    LOGI("AT port %{public}s -> %{public}s", TTY_LINK, slave);
    return master;
}

int main(void)
{
    g_master = OpenPort();
    if (g_master < 0) {
        return 1;
    }
    char line[LINE_MAX_LEN];
    size_t len = 0;
    int64_t nextPoll = 0;

    for (;;) {
        struct pollfd pfd = { g_master, POLLIN, 0 };
        int ret = poll(&pfd, 1, NextTimerMs());
        if (ret < 0 && errno != EINTR) {
            LOGE("poll failed: %{public}d", errno);
            return 1;
        }
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char buf[512];
            ssize_t n = read(g_master, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; i++) {
                char ch = buf[i];
                if (g_smsMode && ch == 0x1A) {
                    line[len] = '\0';
                    SmsPdu(line);
                    Flush();
                    len = 0;
                } else if (g_smsMode && ch == 0x1B) { /* ESC cancels */
                    g_smsMode = 0;
                    len = 0;
                    Out("OK\r\n");
                    Flush();
                } else if (!g_smsMode && (ch == '\r' || ch == '\n')) {
                    if (len > 0) {
                        line[len] = '\0';
                        Command(line);
                        Flush();
                        len = 0;
                    }
                } else if (len < sizeof(line) - 1 && !(g_smsMode && (ch == '\r' || ch == '\n'))) {
                    line[len++] = ch;
                }
            }
        }
        RunTimers();
        if (NowMs() >= nextPoll) {
            PollParams();
            nextPoll = NowMs() + PARAM_POLL_MS;
        }
    }
}
