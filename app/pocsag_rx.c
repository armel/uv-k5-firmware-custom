/* POCSAG receive. See pocsag_rx.h.
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

#include <stddef.h>   // NULL

#include "app/pocsag_rx.h"

// --- codeword arithmetic ---------------------------------------------------

static uint8_t BitCount(uint32_t v)
{
    uint8_t n = 0;

    while (v != 0) {
        v &= v - 1;
        n++;
    }

    return n;
}

static bool Valid(uint32_t cw)
{
    // BchEncode rebuilds the check bits and the parity bit from the 21 payload
    // bits, so a codeword that comes back unchanged is valid in full.
    return POCSAG_BchEncode(cw) == cw;
}

/* Corrects a single bit error, which BCH(31,21) can always do. Two bit errors
 * are correctable in principle as well, but that needs 496 trial pairs per bad
 * codeword against 32 here, and single bit errors are what a marginal signal
 * mostly produces. */
static bool Correct(uint32_t *cw)
{
    if (Valid(*cw))
        return true;

    for (uint8_t i = 0; i < 32; i++) {
        const uint32_t t = *cw ^ (1u << i);

        if (Valid(t)) {
            *cw = t;
            return true;
        }
    }

    return false;
}

// --- message assembly ------------------------------------------------------

static void Append(POCSAG_Rx_t *rx, char c)
{
    if (rx->cap_len >= POCSAG_RX_MAX_CHARS) {
        rx->cap_truncated = true;
        return;
    }

    rx->cap[rx->cap_len++] = c;
}

static void StartCapture(POCSAG_Rx_t *rx, uint32_t ric, uint8_t func)
{
    rx->capturing     = true;
    rx->text_done     = false;
    rx->cap_ric       = ric;
    rx->cap_func      = func;
    rx->char_acc      = 0;
    rx->char_bits     = 0;
    rx->cap_len       = 0;
    rx->cap_truncated = false;
}

// A message ends at the next address codeword, at an idle, or when the
// transmission stops. Returns whether that completed a page.
static bool EndCapture(POCSAG_Rx_t *rx)
{
    if (!rx->capturing)
        return false;

    rx->capturing = false;

    // Pages are commonly padded out to the end of a codeword, so trailing
    // blanks are the sender's filler rather than part of the text.
    while (rx->cap_len > 0 && rx->cap[rx->cap_len - 1] == ' ')
        rx->cap_len--;

    for (uint8_t i = 0; i < rx->cap_len; i++)
        rx->msg[i] = rx->cap[i];

    rx->msg[rx->cap_len] = '\0';
    rx->msg_len          = rx->cap_len;
    rx->page_ric         = rx->cap_ric;
    rx->page_func        = rx->cap_func;
    rx->truncated        = rx->cap_truncated;

    return true;
}

static void AppendPayload(POCSAG_Rx_t *rx, uint32_t payload20)
{
    // Function 3 is alphanumeric on every pager we know of, the rest numeric.
    const uint8_t width = (rx->cap_func == 3) ? 7 : 4;

    for (int8_t b = 19; b >= 0; b--) {
        // The encoder pushes each character least significant bit first, so the
        // first bit on air is bit 0 of the character.
        rx->char_acc |= (uint32_t)((payload20 >> b) & 1u) << rx->char_bits;

        if (++rx->char_bits < width)
            continue;

        const uint8_t value = (uint8_t)(rx->char_acc & ((1u << width) - 1u));

        rx->char_acc  = 0;
        rx->char_bits = 0;

        if (rx->text_done)
            continue;

        if (width == 4) {
            Append(rx, POCSAG_NumericChar(value));
        } else if (value == 0x04) {
            rx->text_done = true;      // end of text, the rest is padding
        } else if (value >= 0x20 && value < 0x7F) {
            Append(rx, (char)value);
        } else {
            Append(rx, ' ');           // control characters, on one LCD line
        }
    }
}

// --- framing ---------------------------------------------------------------

void POCSAG_RxInit(POCSAG_Rx_t *rx, uint32_t ric, bool invert)
{
    rx->ric       = ric;
    rx->invert    = invert;

    rx->shift     = 0;
    rx->have_bits = 0;
    rx->slot      = 0;
    rx->locked    = false;

    rx->capturing = false;
    rx->text_done = false;
    rx->cap_ric   = 0;
    rx->cap_func  = 0;
    rx->char_acc  = 0;
    rx->char_bits = 0;
    rx->cap[0]    = '\0';
    rx->cap_len   = 0;
    rx->cap_truncated = false;

    rx->msg[0]    = '\0';
    rx->msg_len   = 0;
    rx->page_ric  = 0;
    rx->page_func = 0;
    rx->truncated = false;

    rx->codewords = 0;
    rx->corrected = 0;
    rx->bad       = 0;
    rx->hunt_bits = 0;
    rx->idles     = 0;
}

void POCSAG_RxResync(POCSAG_Rx_t *rx, bool invert)
{
    rx->invert    = invert;
    rx->shift     = 0;
    rx->have_bits = 0;
    rx->slot      = 0;
    rx->locked    = true;
    rx->hunt_bits = 0;

    rx->capturing = false;
    rx->char_acc  = 0;
    rx->char_bits = 0;
}

static POCSAG_RxEvent_t OnCodeword(POCSAG_Rx_t *rx, uint32_t cw)
{
    if (rx->slot >= POCSAG_SLOTS_PER_BATCH) {
        // Batches run back to back with a sync word between them and no fresh
        // preamble, so the modem only ever reports the first one. This is where
        // the next has to be, and it is also the check that we are still in step
        // with the transmission.
        if (BitCount(cw ^ POCSAG_SYNC_CODEWORD) <= POCSAG_RX_SYNC_TOLERANCE) {
            rx->slot = 0;
            return POCSAG_RX_EVENT_NONE;
        }

        rx->locked    = false;
        rx->hunt_bits = 0;

        return EndCapture(rx) ? POCSAG_RX_EVENT_PAGE
                              : POCSAG_RX_EVENT_FRAMING_LOST;
    }

    const uint8_t slot = rx->slot++;

    uint32_t fixed = cw;

    if (!Correct(&fixed)) {
        rx->bad++;
        return POCSAG_RX_EVENT_NONE;   // drop it and keep the framing
    }

    rx->codewords++;
    if (fixed != cw)
        rx->corrected++;

    if (fixed == POCSAG_IDLE_CODEWORD) {
        rx->idles++;
        return EndCapture(rx) ? POCSAG_RX_EVENT_PAGE : POCSAG_RX_EVENT_NONE;
    }

    if ((fixed & 0x80000000u) != 0) {
        if (rx->capturing)
            AppendPayload(rx, (fixed >> 11) & 0xFFFFFu);

        return POCSAG_RX_EVENT_NONE;
    }

    // An address codeword: ours or not, it ends whatever came before it.
    const bool completed = EndCapture(rx);

    // The three low bits of the address are not transmitted. They are the frame
    // number, which is the slot the codeword arrived in, so an address is only
    // ever recovered as the one address that could have been sent in this slot.
    const uint32_t ric  = (((fixed >> 13) & 0x3FFFFu) << 3) | (slot >> 1);
    const uint8_t  func = (uint8_t)((fixed >> 11) & 3u);

    // POCSAG_RX_MONITOR_RIC keeps everything, for watching a channel rather than
    // waiting on one address.
    if (rx->ric == POCSAG_RX_MONITOR_RIC || ric == rx->ric)
        StartCapture(rx, ric, func);

    return completed ? POCSAG_RX_EVENT_PAGE : POCSAG_RX_EVENT_NONE;
}

static POCSAG_RxEvent_t OnBit(POCSAG_Rx_t *rx, bool bit)
{
    rx->shift = (rx->shift << 1) | (bit ? 1u : 0u);

    if (!rx->locked) {
        // Hunt for the sync word a bit at a time. Getting back in step this way
        // saves the rest of a transmission that a noise burst knocked us out of.
        rx->hunt_bits++;

        if (BitCount(rx->shift ^ POCSAG_SYNC_CODEWORD) <= POCSAG_RX_SYNC_TOLERANCE) {
            rx->locked    = true;
            rx->slot      = 0;
            rx->have_bits = 0;
            rx->hunt_bits = 0;
        }

        return POCSAG_RX_EVENT_NONE;
    }

    if (++rx->have_bits < 32)
        return POCSAG_RX_EVENT_NONE;

    rx->have_bits = 0;

    return OnCodeword(rx, rx->shift);
}

POCSAG_RxEvent_t POCSAG_RxFeed(POCSAG_Rx_t *rx, const uint8_t *bytes, uint32_t len)
{
    POCSAG_RxEvent_t best = POCSAG_RX_EVENT_NONE;

    for (uint32_t i = 0; i < len; i++) {
        const uint8_t byte = rx->invert ? (uint8_t)~bytes[i] : bytes[i];

        for (int8_t b = 7; b >= 0; b--) {
            const POCSAG_RxEvent_t ev = OnBit(rx, ((byte >> b) & 1u) != 0);

            // The whole buffer is always consumed. Stopping partway would mean
            // resuming mid byte, and the caller can only hand back whole bytes.
            if (ev == POCSAG_RX_EVENT_PAGE || best == POCSAG_RX_EVENT_NONE)
                best = ev;
        }
    }

    return best;
}

POCSAG_RxEvent_t POCSAG_RxFlush(POCSAG_Rx_t *rx)
{
    return EndCapture(rx) ? POCSAG_RX_EVENT_PAGE : POCSAG_RX_EVENT_NONE;
}

#ifndef POCSAG_RX_HOST_TEST

// --- radio session ---------------------------------------------------------

#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"
#include "audio.h"

/* How much data to ask the modem for in one packet. The chip stops at the
 * programmed length and only hunts for a fresh sync word once re-armed, so this
 * is also how long a single transmission can be before its tail is lost: 2040
 * bytes is 34 batches, around 13.6 seconds at 1200 baud. */
#define RX_DATA_BYTES        2040u

// Enough preamble to lock the bit clock without being so much that a receiver
// opening late on a transmission misses the window. POCSAG sends 72 bytes of it.
#define RX_PREAMBLE_BYTES    7u

// Give up hunting after two batches' worth of bits and re-arm the chip instead.
// Past the end of a transmission the demodulator slices noise into bytes
// forever, so without this the receiver would stay deaf until the programmed
// length ran out.
#define RX_HUNT_GIVEUP_BITS  1100u

// Polls with no data at all, after which the transmission is treated as over.
// A chunk is 8 bytes, 53ms at 1200 baud.
#define RX_STALL_POLLS       50u

/* How far the signal has to fall below where it was when the sync word matched
 * before the carrier counts as gone. Raw RSSI is half a dB per step, so this is
 * 10dB, well outside any fade a real transmission would show.
 *
 * This exists because the demodulator does not stop when the carrier does: it
 * goes on slicing receiver noise into bytes, and the chip goes on filling the
 * FIFO until its programmed length runs out. A two-radio test showed 352 bytes
 * arriving from a transmission that only carried 64, and enough of that garbage
 * survived BCH with single bit correction to be decoded as addresses that were
 * never sent. Waiting out the hunt instead took about a second, during which the
 * receiver was deaf to the next page. */
#define RX_CARRIER_DROP      20u

/* The chip's demodulator and POCSAG disagree about which frequency is a one, so
 * the data arrives inverted.
 *
 * Measured rather than assumed. Every way of reading the FIFO - bytes as they
 * come or transposed, data as it comes or inverted - was counted for exact idle
 * codewords against one real transmission between two radios. The inverted
 * reading found nine and the other three found none, which is decisive: an idle
 * is 0x7A89C197 or it is not, and nothing but a real batch produces that.
 *
 * REG_0B<7:6> are documented as reporting whether the sync word matched positive
 * or negative, and the polarity was taken from them at first. They do not give a
 * usable answer: the reading that worked is the one those bits said was not
 * needed. The driver still reports them, since the register list says what they
 * are, but nothing depends on it. */
#define RX_DATA_INVERTED     true

static POCSAG_Rx_t          gRx;
static BK4819_HwFskConfig_t gCfg;
static uint32_t             gFrequency;
static bool                 gRunning;
static bool                 gRearm;
static POCSAG_RxFilter_t    gFilter = POCSAG_RX_FILTER_WIDE;
static uint16_t             gStall;

// Signal level when the sync word matched, as the reference the carrier is
// judged to have fallen away from. Zero means no packet is in progress, so the
// check is not armed.
static uint16_t             gSyncRssi;

static void Config(uint32_t baud)
{
    gCfg.baud           = baud;
    gCfg.preamble_bytes = RX_PREAMBLE_BYTES;
    gCfg.sync_bytes     = 4;

    // POCSAG's frame sync codeword, 0x7CD215D8, as the modem's sync word. Byte 0
    // goes in the high half of REG_5A and is transmitted first, which is the
    // opposite order to the FIFO.
    gCfg.sync01         = (uint16_t)(POCSAG_SYNC_CODEWORD >> 16);
    gCfg.sync23         = (uint16_t)(POCSAG_SYNC_CODEWORD & 0xFFFFu);

    gCfg.deviation      = 0;       // transmit only
    gCfg.gain           = 0;       // transmit only
    gCfg.crc            = false;   // POCSAG carries its own BCH
    gCfg.scramble       = false;

    // The chip is left to hand the data over as it finds it; the inversion
    // POCSAG needs is applied in software instead. See RX_DATA_INVERTED.
    gCfg.invert         = false;
}

/* Turns the receiver on at our frequency. SetFrequency writes REG_38 and REG_39
 * but the synthesiser only takes them on a REG_30 re-trigger, which is what
 * RX_TurnOn does, so the order matters: this cost a bench session on transmit.
 *
 * The audio is muted because the modem taps the discriminator ahead of the
 * audio path, so there is nothing to gain from letting 1200 baud FSK out of the
 * speaker. */
static void RxOn(void)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

    // Silence, both ways: the modem reads the discriminator, so the speaker has
    // nothing to contribute but the sound of 1200 baud FSK.
    BK4819_SetAF(BK4819_AF_MUTE);
    AUDIO_AudioPathOff();

    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_ExitSubAu();

    BK4819_SetFrequency(gFrequency);
    BK4819_PickRXFilterPathBasedOnFrequency(gFrequency);

    /* Selectable, because narrow should fit at 512 and 1200 baud and pass less
     * noise with it; see POCSAG_RxFilter_t. Wide remains the default until that is
     * measured on a signal weak enough for the difference to show. */
    BK4819_SetFilterBandwidth((gFilter == POCSAG_RX_FILTER_NARROW)
                              ? BK4819_FILTER_BW_NARROW : BK4819_FILTER_BW_WIDE,
                              false);

    BK4819_RX_TurnOn();
}

static void Rearm(void)
{
    BK4819_HwFskRxRestart(&gCfg);

    POCSAG_RxInit(&gRx, gRx.ric, RX_DATA_INVERTED);

    gStall    = 0;
    gSyncRssi = 0;
    gRearm    = false;
}

void POCSAG_RX_Start(uint32_t frequency, uint32_t baud, uint32_t ric)
{
    gFrequency = frequency;

    Config(baud);
    POCSAG_RxInit(&gRx, ric, RX_DATA_INVERTED);

    gStall          = 0;
    gSyncRssi       = 0;
    gRearm          = false;
    gRunning        = false;

    RxOn();

    if (BK4819_HwFskRxSetup(&gCfg, RX_DATA_BYTES) != BK4819_HWFSK_OK)
        return;

    gRunning = true;
}

void POCSAG_RX_SetFilter(POCSAG_RxFilter_t filter)
{
    gFilter = filter;
}

POCSAG_RxFilter_t POCSAG_RX_GetFilter(void)
{
    return gFilter;
}

void POCSAG_RX_GetQuality(uint16_t *codewords, uint16_t *corrected, uint16_t *bad)
{
    if (codewords != NULL) *codewords = (uint16_t)gRx.codewords;
    if (corrected != NULL) *corrected = (uint16_t)gRx.corrected;
    if (bad       != NULL) *bad       = (uint16_t)gRx.bad;
}

void POCSAG_RX_Stop(void)
{
    if (!gRunning)
        return;

    BK4819_HwFskStop();
    gRunning = false;
}

const POCSAG_Rx_t *POCSAG_RX_State(void)
{
    return &gRx;
}

bool POCSAG_RX_InSync(void)
{
    return gRunning && gRx.locked;
}

POCSAG_RxEvent_t POCSAG_RX_Poll(void)
{
    if (!gRunning)
        return POCSAG_RX_EVENT_NONE;

    // Deferred from the previous call so that a decoded page survives long
    // enough for the caller to read it out of the state.
    if (gRearm)
        Rearm();

    const uint16_t rssi = BK4819_GetRSSI();

    uint8_t               data[BK4819_HWFSK_RX_CHUNK * 4];
    BK4819_HwFskRxEvent_t chip;
    const uint8_t         got = BK4819_HwFskRxPoll(data, sizeof(data), &chip);

    if (chip.sync) {
        // The modem hands over at the first data bit after the sync word, so the
        // decoder starts a batch here, already aligned.
        POCSAG_RxResync(&gRx, RX_DATA_INVERTED);
        gStall = 0;

        // The carrier is certainly present at this instant, so this is the level
        // everything after it gets judged against.
        gSyncRssi = (rssi == 0) ? 1 : rssi;   // 0 is the "not armed" marker
    }

    POCSAG_RxEvent_t ev = POCSAG_RX_EVENT_NONE;

    if (got > 0) {
        gStall = 0;
        ev     = POCSAG_RxFeed(&gRx, data, got);
    } else if (gRx.locked || gRx.hunt_bits > 0) {
        // Nothing at all is arriving, so the transmission is over. Finish off
        // whatever was in progress rather than waiting for an idle codeword that
        // is never going to come.
        if (++gStall >= RX_STALL_POLLS) {
            gRearm = true;
            return POCSAG_RxFlush(&gRx);
        }
    }

    /* The carrier has gone, so whatever the chip hands over from here on is the
     * demodulator slicing noise, and none of it should be decoded. Checked after
     * feeding rather than before, so that a packet already sitting in the FIFO
     * when the transmitter unkeys is still read out. */
    if (gSyncRssi != 0 && (rssi + RX_CARRIER_DROP) < gSyncRssi) {
        gRearm = true;

        if (ev == POCSAG_RX_EVENT_NONE)
            ev = POCSAG_RxFlush(&gRx);

        return ev;
    }

    // Either the packet ran to its programmed length, or we have been hunting
    // for a sync word long enough to conclude that what is arriving is noise.
    // Both mean the chip has to be re-armed before it will hear anything new.
    if (chip.finished || gRx.hunt_bits > RX_HUNT_GIVEUP_BITS) {
        gRearm = true;

        if (ev == POCSAG_RX_EVENT_NONE)
            ev = POCSAG_RxFlush(&gRx);
    }

    return ev;
}

#endif
