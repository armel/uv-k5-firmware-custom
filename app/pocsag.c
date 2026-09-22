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

#include <stddef.h>   // NULL

#include "app/pocsag.h"

// Index is the 4 bit on-air value, matching the usual numeric paging alphabet.
static const char numericMap[] = "0123456789*U -)(";

struct encoder_t {
    POCSAG_BitBuf_t *buf;
    uint8_t          slot;   // 0 ~ 15, codeword position inside the current batch
};

void POCSAG_BufInit(POCSAG_BitBuf_t *buf, uint8_t *storage, uint32_t capacity_bytes)
{
    buf->data          = storage;
    buf->capacity_bits = capacity_bytes * 8;
    buf->length        = 0;
    buf->overflow      = false;

    for (uint32_t i = 0; i < capacity_bytes; i++)
        storage[i] = 0;
}

bool POCSAG_BufGetBit(const POCSAG_BitBuf_t *buf, uint32_t index)
{
    if (index >= buf->length)
        return false;
    return (buf->data[index >> 3] >> (7 - (index & 7))) & 1u;
}

static void PushBit(POCSAG_BitBuf_t *buf, bool bit)
{
    if (buf->length >= buf->capacity_bits) {
        buf->overflow = true;
        return;
    }

    if (bit)
        buf->data[buf->length >> 3] |= 1u << (7 - (buf->length & 7));
    else
        buf->data[buf->length >> 3] &= ~(1u << (7 - (buf->length & 7)));

    buf->length++;
}

static void PushWord(POCSAG_BitBuf_t *buf, uint32_t word)
{   // MSB first, as transmitted
    for (int i = 31; i >= 0; i--)
        PushBit(buf, (word >> i) & 1u);
}

uint32_t POCSAG_BchEncode(uint32_t codeword)
{
    uint32_t cw  = codeword & 0xFFFFF800u;   // keep the 21 payload bits only
    uint32_t rem = cw;

    // Remainder of the payload polynomial divided by the BCH(31,21) generator.
    // POCSAG_BCH_POLY has degree 10, so shifting it by (i - 10) lines its
    // leading term up with bit i and never reaches down into bit 0.
    for (int i = 31; i >= 11; i--)
        if (rem & (1u << i))
            rem ^= POCSAG_BCH_POLY << (i - 10);

    cw |= rem & 0x000007FEu;

    // trailing even parity over the whole 32 bit word
    uint32_t p = cw;
    p ^= p >> 16;
    p ^= p >> 8;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;

    return cw | (p & 1u);
}

uint32_t POCSAG_AddressCodeword(uint32_t ric, POCSAG_Function_t func)
{   // flag bit 31 = 0, address <30:13>, function <12:11>
    // The low 3 bits of the RIC are not transmitted, they select the frame.
    return POCSAG_BchEncode((((ric >> 3) & 0x3FFFFu) << 13) | ((uint32_t)(func & 3) << 11));
}

uint32_t POCSAG_MessageCodeword(uint32_t payload20)
{   // flag bit 31 = 1, data <30:11>
    return POCSAG_BchEncode(0x80000000u | ((payload20 & 0xFFFFFu) << 11));
}

static void EmitCodeword(struct encoder_t *enc, uint32_t cw)
{
    if (enc->slot == 0)
        PushWord(enc->buf, POCSAG_SYNC_CODEWORD);

    PushWord(enc->buf, cw);

    enc->slot = (enc->slot + 1) & (POCSAG_SLOTS_PER_BATCH - 1);
}

static void PadToSlot(struct encoder_t *enc, uint8_t target)
{   // wraps into the next batch if we are already past the target
    while (enc->slot != target)
        EmitCodeword(enc, POCSAG_IDLE_CODEWORD);
}

static uint8_t CharToNumeric(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c == 'u')
        c = 'U';

    for (uint8_t i = 10; i < 16; i++)
        if (numericMap[i] == c)
            return i;

    return 12;   // space
}

char POCSAG_NumericChar(uint8_t value)
{
    return (value < 16) ? numericMap[value] : ' ';
}

uint32_t POCSAG_EstimateBytes(POCSAG_MsgType_t type, uint32_t msg_len)
{
    const uint32_t bits_per_char = (type == POCSAG_MSG_ALPHA)   ? 7 :
                                   (type == POCSAG_MSG_NUMERIC) ? 4 : 0;

    const uint32_t msg_codewords = ((msg_len * bits_per_char) + 19) / 20;

    // worst case the address has to sit in the last frame of a batch, so allow
    // a full batch of leading idle padding, plus the terminating idle and the
    // trailing ones that keep a receiver's FIFO from eating the message
    const uint32_t data_codewords = POCSAG_SLOTS_PER_BATCH + 1 + msg_codewords
                                    + 1 + POCSAG_TAIL_IDLES;
    const uint32_t batches        = (data_codewords + POCSAG_SLOTS_PER_BATCH - 1) / POCSAG_SLOTS_PER_BATCH;
    const uint32_t total_bits     = POCSAG_PREAMBLE_BITS + (batches * (POCSAG_SLOTS_PER_BATCH + 1) * 32);

    return (total_bits + 7) / 8;
}

const uint8_t *POCSAG_SplitPreamble(const uint8_t *encoded,
                                    uint32_t encoded_bits,
                                    uint32_t lead_bytes,
                                    uint32_t *payload_bytes)
{
    if (encoded == NULL || payload_bytes == NULL || lead_bytes == 0)
        return NULL;

    if ((encoded_bits & 7u) != 0)
        return NULL;                       // the split has to be byte aligned

    if (encoded_bits <= lead_bytes * 8u)
        return NULL;                       // nothing would be left to send

    // Refuse unless what the modem replaces really is alternating preamble.
    for (uint32_t i = 0; i < lead_bytes; i++)
        if (encoded[i] != 0xAA)
            return NULL;

    const uint32_t bytes = (encoded_bits / 8u) - lead_bytes;

    if ((bytes & 1u) != 0)
        return NULL;                       // the FIFO is written a word at a time

    *payload_bytes = bytes;

    return encoded + lead_bytes;
}

bool POCSAG_Encode(POCSAG_BitBuf_t *buf,
                   uint32_t ric,
                   POCSAG_Function_t func,
                   POCSAG_MsgType_t type,
                   const char *msg)
{
    struct encoder_t enc = { .buf = buf, .slot = 0 };

    if (ric > POCSAG_MAX_RIC)
        return false;

    for (uint32_t i = 0; i < POCSAG_PREAMBLE_BITS; i++)
        PushBit(buf, (i & 1) == 0);   // 1010...

    PadToSlot(&enc, 2 * (uint8_t)(ric & 7));
    EmitCodeword(&enc, POCSAG_AddressCodeword(ric, func));

    if (type != POCSAG_MSG_TONE && msg != NULL) {
        const uint8_t width = (type == POCSAG_MSG_ALPHA) ? 7 : 4;
        const uint8_t pad   = (type == POCSAG_MSG_ALPHA) ? 0x04u   // EOT
                                                         : 0x0Cu;  // numeric space
        uint32_t      acc      = 0;
        uint8_t       acc_bits = 0;

        for (const char *p = msg; *p != '\0'; p++) {
            const uint8_t value = (type == POCSAG_MSG_ALPHA) ? (uint8_t)(*p & 0x7F)
                                                             : CharToNumeric(*p);

            for (uint8_t b = 0; b < width; b++) {   // least significant bit first
                acc = (acc << 1) | ((value >> b) & 1u);

                if (++acc_bits == 20) {
                    EmitCodeword(&enc, POCSAG_MessageCodeword(acc));
                    acc      = 0;
                    acc_bits = 0;
                }
            }
        }

        // Fill out the last codeword with pad characters rather than zero bits,
        // which would decode as trailing NULs on the pager.
        if (acc_bits > 0) {
            for (uint8_t b = 0; acc_bits < 20; b = (b + 1) % width) {
                acc = (acc << 1) | ((pad >> b) & 1u);
                acc_bits++;
            }
            EmitCodeword(&enc, POCSAG_MessageCodeword(acc));
        }
    }

    // A message is delimited by the next address codeword or by an idle, so
    // always emit at least one idle before stopping. Without it a message that
    // happens to fill the batch exactly leaves the receiver waiting for more
    // and the whole transmission is dropped.
    EmitCodeword(&enc, POCSAG_IDLE_CODEWORD);

    /* Then enough more that a receiver's FIFO cannot strand real content. See
     * POCSAG_TAIL_IDLES: this radio's own receiver loses about six codewords off the
     * end of a capture, and one terminating idle plus batch padding is not reliably
     * more than one codeword of that. */
    for (uint8_t i = 0; i < POCSAG_TAIL_IDLES; i++)
        EmitCodeword(&enc, POCSAG_IDLE_CODEWORD);

    PadToSlot(&enc, 0);   // idle out to the end of the batch

    return !buf->overflow;
}
