/* POCSAG (CCIR Radio Paging Code No. 1) transmit encoder.
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

#ifndef POCSAG_H
#define POCSAG_H

#include <stdbool.h>
#include <stdint.h>

// On air, POCSAG is 2-level FSK: a binary '0' is the positive frequency
// deviation, a binary '1' the negative one (nominally +/-4.5kHz).

#define POCSAG_PREAMBLE_BITS      576         // >= 576 alternating 1010...
#define POCSAG_SYNC_CODEWORD      0x7CD215D8u
#define POCSAG_IDLE_CODEWORD      0x7A89C197u
#define POCSAG_BCH_POLY           0x769u      // x^10+x^9+x^8+x^6+x^5+x^3+1
#define POCSAG_FRAMES_PER_BATCH   8
#define POCSAG_SLOTS_PER_BATCH    (POCSAG_FRAMES_PER_BATCH * 2)
#define POCSAG_MAX_RIC            0x1FFFFFu   // 21 bits

/* Idle codewords appended after the terminating one, so a receiver does not lose the
 * end of the message.
 *
 * The BK4819's receive FIFO raises almost-full at four words and gives no count, so a
 * final partial chunk of fewer than eight bytes can never be read out, and up to a
 * further sixteen can be sitting there when the carrier drops and the capture closes.
 * That strands about 23 bytes, or six codewords, off the end of every transmission.
 *
 * The encoder already emits one terminating idle and pads to the end of the batch, but
 * if that idle lands in the last slot the padding is four bytes and the rest of the
 * loss eats real message content - about fourteen alphanumeric characters. Eight idles
 * covers the worst case.
 *
 * Six, with the terminating idle making seven, covers the 23 bytes.
 *
 * Six and eight cost the same on air, which is worth knowing before tuning the number:
 * any real tail pushes a worst case 40 character page from two batches to three, and
 * the pad-to-batch-end then fills the rest either way. That lands the page at exactly
 * 256 payload bytes, which is the whole TX FIFO - legal, since the driver allows 256,
 * but with no margin. A longer message would fail POCSAG_TX_Send's length check rather
 * than corrupt anything.
 *
 * Costs 213ms at 1200 baud and nothing in compatibility: a pager treats an idle as the
 * end of the message and ignores the rest. The APRS receive path needed exactly this
 * fix for exactly this reason. */
#define POCSAG_TAIL_IDLES         6

// The address an idle codeword would decode as, which is therefore not available
// to any pager: a receiver treats it as the end of a message, not as a call.
#define POCSAG_IDLE_RIC           ((((POCSAG_IDLE_CODEWORD) >> 13) & 0x3FFFFu) << 3)

enum POCSAG_Function_t {
    POCSAG_FUNC_A = 0,    // tone only on most pagers
    POCSAG_FUNC_B = 1,
    POCSAG_FUNC_C = 2,
    POCSAG_FUNC_D = 3     // alphanumeric on most pagers
};
typedef enum POCSAG_Function_t POCSAG_Function_t;

enum POCSAG_MsgType_t {
    POCSAG_MSG_TONE = 0,  // address only, the pager just beeps
    POCSAG_MSG_NUMERIC,   // 4 bit BCD
    POCSAG_MSG_ALPHA      // 7 bit ASCII
};
typedef enum POCSAG_MsgType_t POCSAG_MsgType_t;

// Packed MSB-first bit buffer. One entry per 8 on-air bits, so the TX bit
// clock reads bit i as (data[i >> 3] >> (7 - (i & 7))) & 1.
struct POCSAG_BitBuf_t {
    uint8_t  *data;
    uint32_t  capacity_bits;
    uint32_t  length;       // bits written so far
    bool      overflow;
};
typedef struct POCSAG_BitBuf_t POCSAG_BitBuf_t;

void     POCSAG_BufInit(POCSAG_BitBuf_t *buf, uint8_t *storage, uint32_t capacity_bytes);
bool     POCSAG_BufGetBit(const POCSAG_BitBuf_t *buf, uint32_t index);

// Fills the BCH(31,21) check bits and the trailing even parity bit of a
// codeword whose 21 payload bits sit in <31:11>. Bits <10:0> are overwritten.
uint32_t POCSAG_BchEncode(uint32_t codeword);

uint32_t POCSAG_AddressCodeword(uint32_t ric, POCSAG_Function_t func);
uint32_t POCSAG_MessageCodeword(uint32_t payload20);

// The character for a 4 bit numeric paging value. Values above 15 give a space.
// Shared with the receive path so both directions use one alphabet.
char     POCSAG_NumericChar(uint8_t value);

// Worst case buffer size in bytes for a message of msg_len characters.
uint32_t POCSAG_EstimateBytes(POCSAG_MsgType_t type, uint32_t msg_len);

// Compile time form of POCSAG_EstimateBytes for alphanumeric messages, for
// sizing static buffers. Callers should still check POCSAG_EstimateBytes at
// runtime rather than trust this to stay in step.
#define POCSAG_ALPHA_BUF_BYTES(chars)                                        \
    ((POCSAG_PREAMBLE_BITS +                                                 \
      ((((POCSAG_SLOTS_PER_BATCH + 2 + POCSAG_TAIL_IDLES +                    \
         ((((chars) * 7) + 19) / 20))                                        \
         + POCSAG_SLOTS_PER_BATCH - 1) / POCSAG_SLOTS_PER_BATCH)             \
       * (POCSAG_SLOTS_PER_BATCH + 1) * 32) + 7) / 8)

// Splits an encoded transmission for a modem that generates the start of the
// preamble itself. Returns a pointer into encoded just past lead_bytes and
// writes the remaining length to payload_bytes.
//
// Returns NULL unless the split is safe: the stream must be byte aligned, long
// enough, leave an even number of payload bytes, and the part being replaced
// must really be alternating 0xAA preamble, otherwise the modem's output and
// the payload would not join up on air.
const uint8_t *POCSAG_SplitPreamble(const uint8_t *encoded,
                                    uint32_t encoded_bits,
                                    uint32_t lead_bytes,
                                    uint32_t *payload_bytes);

// Appends a complete transmission (preamble + batches) to buf. Returns false
// if the buffer overflowed, in which case buf->overflow is also set.
bool     POCSAG_Encode(POCSAG_BitBuf_t *buf,
                       uint32_t ric,
                       POCSAG_Function_t func,
                       POCSAG_MsgType_t type,
                       const char *msg);

#endif
