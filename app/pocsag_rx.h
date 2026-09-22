/* POCSAG receive: a streaming decoder, and the radio session that feeds it.
 *
 * The BK4819's FSK receiver is pointed at POCSAG's own frame sync word, so the
 * chip does the clock recovery, the sync hunt and the byte framing, and hands
 * over the data that follows. Everything above that - codewords, BCH, batch
 * structure, addresses and text - is done here.
 *
 * The decoder half is deliberately free of driver calls so it can be run on a
 * host against the transmit encoder's own output. Only the session half below
 * touches the radio.
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

#ifndef POCSAG_RX_H
#define POCSAG_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "app/pocsag.h"

// Longest message kept. Pages are truncated rather than dropped, since a cut
// off message still tells the operator who called.
#define POCSAG_RX_MAX_CHARS      60

// How far a codeword may be from the sync word and still count as sync. 2 of 32
// bits is well inside what separates it from any valid codeword, and the sync
// word is the one thing that must never be missed.
#define POCSAG_RX_SYNC_TOLERANCE 2

/* Give the decoder this as its own address and it keeps every page it decodes
 * rather than only the ones addressed to it.
 *
 * Zero is a safe sentinel. To address a pager as 0 a transmitter would have to
 * send an all-zero address codeword in frame 0, which happens to be exactly what
 * a dead carrier decodes as, so no real paging system uses it. Worth knowing
 * though: in this mode that same dead-carrier pattern is no longer filtered out
 * by the address comparison, so a signal that drops mid-batch can produce a
 * "page" from address 0 to 7. The carrier check in the session is what keeps that
 * rare. */
#define POCSAG_RX_MONITOR_RIC    0u

/* The IF filter bandwidth to receive with.
 *
 * Wide is what this has always used, and it has never been compared against
 * narrow, which the arithmetic says should fit: POCSAG's nominal +/-4.5kHz
 * deviation gives a Carson bandwidth of 2*(4.5+0.512)=10kHz at 512 baud and
 * 11.4kHz at 1200, both inside narrow's 12.5kHz, while 2400 baud needs 13.8kHz
 * and so requires wide.
 *
 * Narrow passes less noise, so it should be more sensitive where it fits. That is
 * untested, and worth testing: on the APRS side the same choice was the difference
 * between 32 of 32 bytes correct and none at all. But it can only be measured on a
 * marginal signal - two radios on a desk decode perfectly either way - so it is a
 * setting rather than a fixed change. */
enum POCSAG_RxFilter_t {
    POCSAG_RX_FILTER_WIDE = 0,
    POCSAG_RX_FILTER_NARROW
};
typedef enum POCSAG_RxFilter_t POCSAG_RxFilter_t;

void POCSAG_RX_SetFilter(POCSAG_RxFilter_t filter);
POCSAG_RxFilter_t POCSAG_RX_GetFilter(void);

// Receive quality since the last Start: codewords seen, how many needed a BCH
// correction, and how many failed it. Corrections are the useful number when
// comparing filter settings, because they rise before decoding actually fails.
void POCSAG_RX_GetQuality(uint16_t *codewords, uint16_t *corrected, uint16_t *bad);

enum POCSAG_RxEvent_t {
    POCSAG_RX_EVENT_NONE = 0,
    POCSAG_RX_EVENT_PAGE,          // a page addressed to us completed
    POCSAG_RX_EVENT_FRAMING_LOST   // the batch sync stopped arriving
};
typedef enum POCSAG_RxEvent_t POCSAG_RxEvent_t;

struct POCSAG_Rx_t {
    // --- configuration
    uint32_t ric;             // the address we answer to
    bool     invert;          // the data arrives inverted

    // --- bit and codeword framing
    uint32_t shift;           // the last 32 bits seen
    uint8_t  have_bits;       // bits accumulated toward the current codeword
    uint8_t  slot;            // 0..15 inside the batch, 16 = the next inter
                              // batch sync is due
    bool     locked;          // false while hunting for a sync word

    // --- message in progress
    bool     capturing;       // inside a message addressed to us
    bool     text_done;       // an end of text character has been seen
    uint32_t cap_ric;
    uint8_t  cap_func;        // function bits of the address codeword
    uint32_t char_acc;
    uint8_t  char_bits;
    char     cap[POCSAG_RX_MAX_CHARS + 1];
    uint8_t  cap_len;
    bool     cap_truncated;

    /* --- the page just decoded, valid after POCSAG_RX_EVENT_PAGE
     *
     * Kept apart from the message being assembled rather than being the same
     * buffer. A page ends at the next address codeword, and that codeword can be
     * the start of another page to us in the very next slot, which would
     * otherwise clear the message that had just been completed and report an
     * empty page. */
    char     msg[POCSAG_RX_MAX_CHARS + 1];
    uint8_t  msg_len;
    uint32_t page_ric;
    uint8_t  page_func;
    bool     truncated;       // the message was longer than we can hold

    // --- how well it is decoding, which the host tests assert on
    uint16_t codewords;       // passed BCH, corrected ones included
    uint16_t corrected;       // needed a single bit flip
    uint16_t bad;             // beyond correcting
    uint16_t hunt_bits;       // bits searched since framing was last lost

    /* Idle codewords, counted separately because they are the one thing that says
     * a stream really was POCSAG. Both 0x00000000 and 0xFFFFFFFF are valid BCH
     * codewords, one looking like an address and the other like a message, so a
     * demodulator slicing receiver noise into long runs fills the codeword count
     * with words that were never transmitted. 0x7A89C197 it does not produce. */
    uint16_t idles;
};
typedef struct POCSAG_Rx_t POCSAG_Rx_t;

// --- decoder ---------------------------------------------------------------

// Starts from the first data bit after a sync word, which is where the modem
// hands over, so the decoder begins in frame 0 already aligned.
void POCSAG_RxInit(POCSAG_Rx_t *rx, uint32_t ric, bool invert);

// Re-aligns on a freshly detected sync word, keeping the decoded message and
// the counters. For when the modem reports a new packet rather than the decoder
// finding the sync itself.
void POCSAG_RxResync(POCSAG_Rx_t *rx, bool invert);

// Feeds received data. Bits are taken most significant first within each byte,
// as transmitted. The whole buffer is always consumed; the return value is the
// most significant thing that happened during it, a completed page outranking
// lost framing. If two pages for us complete in one call, which needs both to
// arrive inside the same handful of bytes, the later one is what is kept.
POCSAG_RxEvent_t POCSAG_RxFeed(POCSAG_Rx_t *rx, const uint8_t *bytes, uint32_t len);

// Ends any message in progress, for when the transmission stops rather than
// being terminated by an idle codeword. Returns POCSAG_RX_EVENT_PAGE if that
// completed a page for us.
POCSAG_RxEvent_t POCSAG_RxFlush(POCSAG_Rx_t *rx);

#ifndef POCSAG_RX_HOST_TEST

// --- radio session ---------------------------------------------------------
//
// Non blocking, so a UI can keep reading the keypad while listening. Tune and
// arm once with Start, call Poll from the main loop, and read the message out
// of the state when Poll reports a page.

// Arms the receiver. frequency is in the 10Hz units the chip uses.
void POCSAG_RX_Start(uint32_t frequency, uint32_t baud, uint32_t ric);

// Puts the modem away. Leaves the radio for the caller to restore.
void POCSAG_RX_Stop(void);

// Services the modem and the decoder. Call at least every 50ms at 1200 baud.
POCSAG_RxEvent_t POCSAG_RX_Poll(void);

// The decoder state, including the last message decoded.
const POCSAG_Rx_t *POCSAG_RX_State(void);

// True between a sync word and the framing being lost, which is as close to
// "a transmission is in progress" as the chip will say.
bool POCSAG_RX_InSync(void);

#endif

#endif
