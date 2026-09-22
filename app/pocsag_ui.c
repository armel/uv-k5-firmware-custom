/* POCSAG pager UI: multi-tap text entry, a transmit screen, and a receiver.
 *
 * The screen is a pager as much as a pager terminal. While it is open the
 * radio listens on the same channel it transmits on, and anything addressed to
 * the radio's own RIC is shown on the bottom two rows.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#include "app/pocsag_ui.h"

#include "app/pocsag.h"
#include "app/pocsag_rx.h"
#include "app/pocsag_tx.h"
#include "bsp/dp32g030/syscon.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"

// The small font is 6px wide on a 1px pitch, so a row holds 18 characters
// starting at x=2: 2 + 17*7 + 6 is exactly 127, the last byte of the row.
#define COLS                18
#define TAP_TIMEOUT_10MS    80      // how long the same key keeps cycling
#define REPEAT_DELAY_10MS   60
#define REPEAT_RATE_10MS    10

enum {
    FIELD_OWN = 0,     // the address this radio answers to
    FIELD_FREQ,
    FIELD_RIC,         // the address a page is sent to
    FIELD_BAUD,
    FIELD_MSG,
    FIELD_COUNT
};

/* gFrameBuffer holds FRAME_LINES = 7 rows, drawn as hardware rows 1 to 7; the
 * hardware's row 0 is the separate gStatusLine buffer. So there is no row 7 to
 * write to, and writing one runs off the end of the array.
 *
 * Seven rows is one short of what this screen wants, so the title, the status
 * and the receive indicator go on the status line, which nothing else is
 * maintaining while this screen owns the display. That leaves all seven rows for
 * the five fields and the two a received page needs. */
#define ROW_FIELD0          0
#define ROW_RX              FIELD_COUNT
#define RX_ROWS             2
#define RX_SHOWN            (COLS * RX_ROWS)

// x positions on the status line, spaced so the three pieces cannot run into
// each other or off the end: "POCSAG" ends at 43, an 8 character status at 101,
// a 3 character indicator at 124.
#define STATUS_X_TITLE      2
#define STATUS_X_TEXT       46
#define STATUS_X_RX         104

static const uint32_t gBaudTable[] = { 512, 1200, 2400 };

/* Channels worth reaching in one keypress, stepped with # on the FRQ row.
 *
 *   153.22500  the Philips 53D pager this was developed against, read off the
 *              crystal on its RF board
 *   439.98750  DAPNET, the amateur paging network: live POCSAG 1200 traffic, and
 *              so the only way to test a receiver against a transmitter that
 *              nobody here built
 *
 * Both sit inside the firmware's unconditionally TX-allowed bands. Adding more is
 * one line. */
static const uint32_t gFreqPreset[] = { 15322500, 43998750 };

/* Addresses worth reaching in one keypress, stepped with # on the RIC row.
 *
 *   1069132   \ the two UV-K5s this was developed with, as their chip IDs happen
 *   2086390   / to hash to. Specific to this pair of radios, not to anything else
 *   1578624   the Philips 53D pager: identifier 1 from its own PCD5002 EEPROM
 *
 * Whichever of these matches this radio's OWN address is skipped, since paging
 * yourself is never the intention. */
static const uint32_t gRicPreset[] = { 1069132, 2086390, 1578624 };

// Nokia style multi-tap. Index is the digit key.
static const char *gTapSet[10] = {
    " 0",         // 0
    "1.,-/?!",    // 1
    "ABC2",
    "DEF3",
    "GHI4",
    "JKL5",
    "MNO6",
    "PQRS7",
    "TUV8",
    "WXYZ9",
};

/* The radio's own address. There is no paging authority to allocate one, so it
 * is derived from the MCU's 128 bit chip ID: unique to this radio, and constant
 * across reboots, so the default needs no storage of its own. Changing it here
 * only lasts as long as the radio stays on.
 *
 * SYSCON_CHIP_ID is read once, lazily, rather than in an initialiser, because
 * this is the only thing in the file that depends on the hardware existing. */
static uint32_t OwnRicDefault(void)
{
    const uint32_t id[4] = {
        SYSCON_CHIP_ID0, SYSCON_CHIP_ID1, SYSCON_CHIP_ID2, SYSCON_CHIP_ID3
    };

    // All zeroes or all ones means the registers are not what we think they
    // are, and every radio would end up on the same address. Better an obvious
    // constant that the operator can see and change.
    bool useful = false;

    for (uint8_t i = 0; i < 4; i++)
        if (id[i] != 0 && id[i] != 0xFFFFFFFFu)
            useful = true;

    if (!useful)
        return 1000000;

    uint32_t hash = 2166136261u;   // FNV-1a over the 16 bytes

    for (uint8_t i = 0; i < 4; i++)
        for (uint8_t b = 0; b < 4; b++) {
            hash ^= (id[i] >> (b * 8)) & 0xFFu;
            hash *= 16777619u;
        }

    uint32_t ric = hash & POCSAG_MAX_RIC;

    // Stay out of the addresses test gear favours, and off the one block that
    // cannot be paged because an idle codeword decodes as it.
    if (ric < 16)
        ric += 16;

    if ((ric & ~7u) == POCSAG_IDLE_RIC)
        ric += 8;

    return ric;
}

// Kept across visits so a page can be re-sent without retyping.
//
// The default frequency is 153.2250MHz, read off the crystal of a Philips 53D
// pager (UAA2082H receiver). It sits exactly on the 12.5kHz grid and is inside
// the firmware's TX-allowed BAND3.
static uint32_t gFreq    = 15322500;   // in the 10Hz units the chip uses
// Identifier 1 from the pager's own PCD5002 EEPROM, which the datasheet says
// always holds a RIC. Identifiers 2 and 3 were also enabled, 683607 and 683615,
// eight apart and so in the same frame, which looks like a group pair.
static uint32_t gRic     = 1578624;
static uint32_t gOwnRic;               // see OwnRicDefault
static bool     gOwnRicSet;            // so that clearing it to 0 stays cleared

/* The address to come back to when monitor mode is switched off again. Tracks
 * whatever was last set rather than always returning to the chip ID default, so
 * that an address typed in to match a real network or another radio survives a
 * trip through ALL instead of being quietly discarded. */
static uint32_t gOwnRicSaved;
static uint8_t  gBaudIdx = 1;          // 1200
static char     gMsg[POCSAG_TX_MAX_CHARS + 1] = "HELLO";
static uint8_t  gMsgLen  = 5;

static uint8_t  gField;
// Eight characters plus a terminator, which is what fits between the title and
// the receive indicator on the status line. Sized so that a longer message added
// later cannot silently draw over the indicator.
static char     gStatus[9];
static bool     gRedraw;
static bool     gRunning;

// The last page received, kept across visits like everything else.
// Room for the message plus, when monitoring, the address it was sent to and a
// space in front of it.
#define RX_TEXT_MAX  (POCSAG_RX_MAX_CHARS + 8)

static char     gRxText[RX_TEXT_MAX + 1];
static uint8_t  gRxLen;

// Set whenever something the receiver depends on changes, so that it is re-armed
// once rather than on every pass of the loop.
static bool     gRxRestart;
static bool     gWasInSync;

// multi-tap state
static uint16_t   gTapTimer;
static KEY_Code_t gTapKey;
static uint8_t    gTapIndex;

// digits typed into the selected numeric field; 0 means the next digit starts over
static uint8_t  gEntryLen;

static void SetStatus(const char *s)
{
    strncpy(gStatus, s, sizeof(gStatus) - 1);
    gStatus[sizeof(gStatus) - 1] = '\0';
    gRedraw = true;
}

// --- formatting ------------------------------------------------------------
//
// Hand rolled rather than sprintf: the call sites cost a few hundred bytes of
// flash each once the format strings and argument marshalling are counted, and
// this feature is close enough to the flash limit for that to matter.

static char *AppendStr(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;

    *p = '\0';

    return p;
}

static char *AppendUint(char *p, uint32_t v, uint8_t min_digits)
{
    char    tmp[10];
    uint8_t n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));

    while (n < min_digits && n < sizeof(tmp))
        tmp[n++] = '0';

    while (n > 0)
        *p++ = tmp[--n];

    *p = '\0';

    return p;
}

// --- drawing ---------------------------------------------------------------

static void FormatValue(uint8_t field, char *out)
{
    switch (field) {
        case FIELD_FREQ: {
            char *p = AppendUint(out, gFreq / 100000u, 1);
            p = AppendStr(p, ".");
            AppendUint(p, gFreq % 100000u, 5);
            break;
        }

        case FIELD_OWN:
            // Zero is not an address, it is the instruction to keep every page
            // rather than only ours, so it is worth spelling out.
            if (gOwnRic == POCSAG_RX_MONITOR_RIC)
                AppendStr(out, "ALL");
            else
                AppendUint(out, gOwnRic, 1);
            break;

        case FIELD_RIC:
            AppendUint(out, gRic, 1);
            break;

        case FIELD_BAUD: {
            char *p = out;

            p = AppendUint(p, gBaudTable[gBaudIdx], 1);
            p = AppendStr(p, " ");
            AppendStr(p, (POCSAG_RX_GetFilter() == POCSAG_RX_FILTER_NARROW)
                         ? "NARROW" : "WIDE");
            break;
        }

        default:
            out[0] = '\0';
            break;
    }
}

static void Render(void)
{
    static const char *label[FIELD_COUNT] = {
        "OWN", "FRQ", "RIC", "BPS", "MSG"
    };
    char value[COLS + 2];
    char line[COLS + 2];

    UI_DisplayClear();

    // The title, the transmit result and the receive indicator share the status
    // line, which frees all seven of the real rows for content.
    memset(gStatusLine, 0, LCD_WIDTH);

    UI_PrintStringSmallBufferNormal("POCSAG", gStatusLine + STATUS_X_TITLE);

    if (gStatus[0] != '\0')
        UI_PrintStringSmallBufferNormal(gStatus, gStatusLine + STATUS_X_TEXT);

    // The receiver is on whenever this screen is, so say so, and say when a
    // transmission is actually being tracked rather than just listened for.
    UI_PrintStringSmallBufferNormal(POCSAG_RX_InSync() ? "RX*" : "RX",
                                    gStatusLine + STATUS_X_RX);

    // the five fields, one per row, so nothing needs scrolling
    for (uint8_t f = 0; f < FIELD_COUNT; f++) {
        if (f == FIELD_MSG) {
            const uint8_t room  = COLS - 6;
            const uint8_t shown = (gMsgLen > room) ? room : gMsgLen;

            memcpy(value, gMsg + (gMsgLen - shown), shown);
            value[shown]     = '_';
            value[shown + 1] = '\0';
        } else {
            FormatValue(f, value);
        }

        char *p = line;
        *p++ = (f == gField) ? '>' : ' ';
        p = AppendStr(p, label[f]);
        p = AppendStr(p, " ");
        AppendStr(p, value);

        line[COLS] = '\0';

        UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_FIELD0 + f));
    }

    /* With no page yet, the spare rows carry receive quality instead. Corrections
     * are the number to watch when comparing filter settings: BCH fixes single bit
     * errors silently, so they climb while decoding still succeeds and fall to zero
     * on a strong signal, which makes them the only usable margin indicator short of
     * losing pages outright. */
    if (gRxLen == 0) {
        uint16_t cw, corrected, bad;
        char    *p = line;

        POCSAG_RX_GetQuality(&cw, &corrected, &bad);

        p = AppendStr(p, "C");
        p = AppendUint(p, cw, 1);
        p = AppendStr(p, " FIX");
        p = AppendUint(p, corrected, 1);
        p = AppendStr(p, " BAD");
        AppendUint(p, bad, 1);

        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_RX);
    }

    // The rows the fields do not use hold the last page received.
    for (uint8_t r = 0; r < RX_ROWS && (uint16_t)(r * COLS) < gRxLen; r++) {
        const uint8_t off = (uint8_t)(r * COLS);
        uint8_t       n   = (uint8_t)(gRxLen - off);

        if (n > COLS)
            n = COLS;

        memcpy(line, gRxText + off, n);
        line[n] = '\0';

        // Say when there is more to the page than these rows can show, rather
        // than cutting it off silently.
        if (r == RX_ROWS - 1 && gRxLen > RX_SHOWN)
            line[COLS - 1] = '>';

        UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_RX + r));
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

// --- editing ---------------------------------------------------------------

static void OnDigit(KEY_Code_t key)
{
    const uint8_t d = (uint8_t)key;   // KEY_0 is 0

    switch (gField) {
        case FIELD_MSG: {
            const char    *set = gTapSet[d];
            const uint8_t  n   = (uint8_t)strlen(set);

            if (gTapTimer > 0 && gTapKey == key && gMsgLen > 0) {
                // still inside the window, so cycle the character in place
                gTapIndex      = (uint8_t)((gTapIndex + 1) % n);
                gMsg[gMsgLen - 1] = set[gTapIndex];
            } else {
                if (gMsgLen >= POCSAG_TX_MAX_CHARS) {
                    SetStatus("FULL");
                    return;
                }
                gTapIndex        = 0;
                gMsg[gMsgLen++]  = set[0];
                gMsg[gMsgLen]    = '\0';
                gTapKey          = key;
            }

            gTapTimer = TAP_TIMEOUT_10MS;
            break;
        }

        case FIELD_FREQ:
            if (gEntryLen == 0)
                gFreq = 0;
            if (gEntryLen < 8) {
                gFreq = (gFreq * 10u) + d;
                gEntryLen++;
            }
            gRxRestart = true;
            break;

        case FIELD_OWN:
        case FIELD_RIC: {
            uint32_t *ric = (gField == FIELD_OWN) ? &gOwnRic : &gRic;

            if (gEntryLen == 0)
                *ric = 0;
            if (gEntryLen < 7) {
                *ric = (*ric * 10u) + d;
                gEntryLen++;
            }
            if (*ric > POCSAG_MAX_RIC)
                *ric = POCSAG_MAX_RIC;

            if (gField == FIELD_OWN)
                gRxRestart = true;
            break;
        }

        default:
            break;   // the enumerated fields are changed with the side keys
    }

    gRedraw = true;
}

static void OnBackspace(void)
{
    switch (gField) {
        case FIELD_MSG:
            if (gMsgLen > 0)
                gMsg[--gMsgLen] = '\0';
            gTapTimer = 0;
            break;

        case FIELD_FREQ:
            gFreq /= 10u;
            if (gEntryLen > 0) gEntryLen--;
            gRxRestart = true;
            break;

        case FIELD_OWN:
            gOwnRic /= 10u;
            if (gEntryLen > 0) gEntryLen--;
            gRxRestart = true;
            break;

        case FIELD_RIC:
            gRic /= 10u;
            if (gEntryLen > 0) gEntryLen--;
            break;

        default:
            break;
    }

    gRedraw = true;
}

/* The next preset after whatever is showing, or the first one if the current
 * frequency is not a preset at all. Matching on the value rather than keeping an
 * index means typing or stepping a frequency by hand leaves this predictable:
 * the next # always lands on the first preset rather than somewhere arbitrary in
 * the cycle. */
static uint32_t NextFreqPreset(uint32_t current)
{
    const uint8_t n = (uint8_t)(sizeof(gFreqPreset) / sizeof(gFreqPreset[0]));

    for (uint8_t i = 0; i < n; i++)
        if (gFreqPreset[i] == current)
            return gFreqPreset[(i + 1) % n];

    return gFreqPreset[0];
}

/* As NextFreqPreset, except that the radio's own address is stepped over. On a
 * pair of radios that leaves the other radio and the pager, which is exactly the
 * set worth cycling. */
static uint32_t NextRicPreset(uint32_t current)
{
    const uint8_t n     = (uint8_t)(sizeof(gRicPreset) / sizeof(gRicPreset[0]));
    uint8_t       start = 0;

    for (uint8_t i = 0; i < n; i++)
        if (gRicPreset[i] == current) {
            start = (uint8_t)(i + 1);
            break;
        }

    for (uint8_t k = 0; k < n; k++) {
        const uint32_t ric = gRicPreset[(start + k) % n];

        if (ric != gOwnRic)
            return ric;
    }

    return current;   // every preset is ours, which takes more radios than exist
}

static void OnClear(void)
{
    /* On the rate row, # switches the IF filter rather than clearing anything.
     * Narrow should fit at 512 and 1200 baud and pass less noise with it, which is
     * untested and only measurable on a weak signal - so it is here to be compared,
     * not preset. See POCSAG_RxFilter_t. */
    if (gField == FIELD_BAUD) {
        POCSAG_RX_SetFilter((POCSAG_RX_GetFilter() == POCSAG_RX_FILTER_WIDE)
                            ? POCSAG_RX_FILTER_NARROW : POCSAG_RX_FILTER_WIDE);
        gRxRestart = true;
        gRedraw    = true;
        return;
    }

    switch (gField) {
        case FIELD_MSG:  gMsgLen = 0; gMsg[0] = '\0'; gTapTimer = 0; break;

        // Not cleared to zero, which is not a frequency anything can do: # steps
        // the presets instead, which is what the key is actually wanted for here.
        case FIELD_FREQ:
            gFreq      = NextFreqPreset(gFreq);
            gEntryLen  = 0;
            gRxRestart = true;
            break;

        /* Toggles monitor mode rather than just clearing, so there is a way back:
         * ALL is reachable and so is the address that was in use before it. */
        case FIELD_OWN:
            if (gOwnRic == POCSAG_RX_MONITOR_RIC) {
                gOwnRic = (gOwnRicSaved != POCSAG_RX_MONITOR_RIC) ? gOwnRicSaved
                                                                  : OwnRicDefault();
            } else {
                gOwnRicSaved = gOwnRic;
                gOwnRic      = POCSAG_RX_MONITOR_RIC;
            }

            gEntryLen  = 0;
            gRxRestart = true;
            break;

        // Stepped, not cleared, for the same reason as FRQ: zero is not an
        // address anything answers to.
        case FIELD_RIC:
            gRic      = NextRicPreset(gRic);
            gEntryLen = 0;
            break;

        default: break;
    }

    gRedraw = true;
}

static void StepRic(uint32_t *ric, int8_t dir)
{
    if (dir > 0)
        *ric = (*ric < POCSAG_MAX_RIC) ? (*ric + 1) : 0;
    else
        *ric = (*ric > 0) ? (*ric - 1) : POCSAG_MAX_RIC;
}

static void OnAdjust(int8_t dir)
{
    switch (gField) {
        case FIELD_FREQ: {
            const uint32_t step = 1250;   // 12.5kHz channel spacing
            if (dir > 0)
                gFreq += step;
            else
                gFreq = (gFreq > step) ? (gFreq - step) : 0;
            gEntryLen  = 0;
            gRxRestart = true;
            break;
        }

        case FIELD_OWN:
            StepRic(&gOwnRic, dir);
            gEntryLen  = 0;
            gRxRestart = true;
            break;

        case FIELD_RIC:
            StepRic(&gRic, dir);
            gEntryLen = 0;
            break;

        case FIELD_BAUD: {
            const uint8_t n = (uint8_t)(sizeof(gBaudTable) / sizeof(gBaudTable[0]));
            gBaudIdx   = (uint8_t)((gBaudIdx + (dir > 0 ? 1 : n - 1)) % n);
            gRxRestart = true;
            break;
        }

        default:
            break;
    }

    gRedraw = true;
}

// --- receive ---------------------------------------------------------------

static void StartReceive(void)
{
    POCSAG_RX_Start(gFreq, gBaudTable[gBaudIdx], gOwnRic);
    gRxRestart = false;
}

static void OnPage(void)
{
    const POCSAG_Rx_t *rx = POCSAG_RX_State();
    const bool         monitoring = (gOwnRic == POCSAG_RX_MONITOR_RIC);
    char              *p = gRxText;

    // When monitoring, who the page was for is the interesting part: without it
    // the text is a message from nobody.
    if (monitoring) {
        p = AppendUint(p, rx->page_ric, 1);
        *p++ = ' ';
    }

    if (rx->msg_len > 0) {
        uint8_t n = rx->msg_len;

        if (n > (uint8_t)(RX_TEXT_MAX - (p - gRxText)))
            n = (uint8_t)(RX_TEXT_MAX - (p - gRxText));

        memcpy(p, rx->msg, n);
        p += n;
        *p = '\0';
    } else {
        // An address with no message is a plain alert, which is all a tone only
        // pager ever gets. Say that rather than showing nothing.
        p = AppendStr(p, "(ALERT)");
    }

    gRxLen = (uint8_t)(p - gRxText);

    SetStatus(rx->truncated ? "PAGE +" : "PAGE");

    BACKLIGHT_TurnOn();
    Render();   // on screen before any alert, which blocks for a moment

    /* A pager that does not make a noise is not much use, but only when the page
     * was actually for us. Monitoring a busy channel would otherwise beep
     * continuously, and worse, each alert blocks for the best part of half a
     * second and would lose whatever arrived meanwhile.
     *
     * This reconfigures the chip, so the receiver is re-armed afterwards either
     * way. */
    if (!monitoring) {
        BK4819_PlaySingleTone(1000, 150, 96, true);
        SYSTEM_DelayMs(80);
        BK4819_PlaySingleTone(1000, 150, 96, true);
    }

    gRxRestart = true;
    gRedraw    = false;
}

// --- transmit --------------------------------------------------------------

static const char *ResultText(POCSAG_TxResult_t r)
{
    switch (r) {
        case POCSAG_TX_OK:                 return "SENT";
        case POCSAG_TX_ERR_TX_NOT_ALLOWED: return "NO TX";
        case POCSAG_TX_ERR_TOO_LONG:       return "TOO LONG";
        case POCSAG_TX_ERR_MODEM:          return "MODEM";
        default:                           return "ERR";
    }
}

static void Transmit(void)
{
    // Respect the radio's own TX power menu rather than forcing a level, so
    // the operator keeps control of how much gets radiated. A bias of zero
    // means the PA produces nothing at all, which is never what was wanted, so
    // fall back to a low but non-zero value rather than transmitting silence.
    uint8_t bias = (gCurrentVfo != NULL) ? gCurrentVfo->TXP_CalculatedSetting : 0;

    if (bias == 0)
        bias = 0x20;

    gEntryLen = 0;
    gTapTimer = 0;

    SetStatus("SENDING");
    Render();   // show it before we block for the length of the burst

    POCSAG_RX_Stop();

    // An empty message is a request to make the pager beep, which is a tone
    // only page rather than an alphanumeric one with nothing in it.
    const bool tone = (gMsgLen == 0);

    const POCSAG_TxResult_t r =
        POCSAG_TX_Send(gFreq, gBaudTable[gBaudIdx], gRic,
                       tone ? POCSAG_FUNC_A : POCSAG_FUNC_D,
                       tone ? POCSAG_MSG_TONE : POCSAG_MSG_ALPHA,
                       gMsg, bias, NULL);

    SetStatus(ResultText(r));

    StartReceive();
}

// --- keys ------------------------------------------------------------------

static void OnKeyPress(KEY_Code_t key)
{
    BACKLIGHT_TurnOn();

    switch (key) {
        case KEY_0 ... KEY_9:
            OnDigit(key);
            break;

        case KEY_UP:
            gField    = (uint8_t)((gField + FIELD_COUNT - 1) % FIELD_COUNT);
            gEntryLen = 0;
            gTapTimer = 0;
            gRedraw   = true;
            break;

        case KEY_DOWN:
            gField    = (uint8_t)((gField + 1) % FIELD_COUNT);
            gEntryLen = 0;
            gTapTimer = 0;
            gRedraw   = true;
            break;

        case KEY_SIDE1:
            OnAdjust(+1);
            break;

        case KEY_SIDE2:
            OnAdjust(-1);
            break;

        case KEY_STAR:
            OnBackspace();
            break;

        case KEY_F:
            OnClear();
            break;

        case KEY_MENU:
            Transmit();
            break;

        case KEY_EXIT:
            gRunning = false;
            break;

        default:
            break;
    }
}

static bool Repeatable(KEY_Code_t key)
{
    return key == KEY_UP || key == KEY_DOWN || key == KEY_SIDE1 || key == KEY_SIDE2;
}

void APP_RunPocsag(void)
{
    KEY_Code_t rawPrev     = KEY_INVALID;
    KEY_Code_t stableKey   = KEY_INVALID;
    uint8_t    stableCount = 0;
    uint16_t   heldTicks   = 0;

    if (!gOwnRicSet) {
        gOwnRic    = OwnRicDefault();
        gOwnRicSet = true;
    }

    gField   = FIELD_MSG;
    gRunning = true;
    gRedraw  = true;
    gStatus[0] = '\0';
    gEntryLen  = 0;
    gTapTimer  = 0;

    BACKLIGHT_TurnOn();
    StartReceive();

    while (gRunning) {
        if (!gNextTimeslice) {
            if (gRedraw) {
                gRedraw = false;
                Render();
            }
            continue;
        }

        gNextTimeslice = false;

        if (gTapTimer > 0)
            gTapTimer--;

        if (gRxRestart)
            StartReceive();

        switch (POCSAG_RX_Poll()) {
            case POCSAG_RX_EVENT_PAGE:
                OnPage();
                break;

            default:
                break;
        }

        // The sync indicator changes on its own, so it needs its own trigger to
        // get redrawn rather than waiting for a keypress.
        const bool sync = POCSAG_RX_InSync();

        if (sync != gWasInSync) {
            gWasInSync = sync;
            gRedraw    = true;
        }

        // Debounce by requiring the same reading twice, then act once on the
        // edge rather than for as long as the key is down.
        const KEY_Code_t raw = KEYBOARD_Poll();

        if (raw != rawPrev) {
            rawPrev     = raw;
            stableCount = 0;
            continue;
        }

        if (stableCount < 2) {
            stableCount++;

            if (stableCount == 2 && raw != stableKey) {
                stableKey = raw;
                heldTicks = 0;

                if (raw != KEY_INVALID)
                    OnKeyPress(raw);
            }
            continue;
        }

        // held: repeat the keys where holding is useful
        if (stableKey != KEY_INVALID && Repeatable(stableKey)) {
            heldTicks++;

            if (heldTicks > REPEAT_DELAY_10MS && (heldTicks % REPEAT_RATE_10MS) == 0)
                OnKeyPress(stableKey);
        }
    }

    POCSAG_RX_Stop();

    // hand the radio back to the main screen
    RADIO_SetupRegisters(true);

    gUpdateDisplay = true;
    gUpdateStatus  = true;
}
