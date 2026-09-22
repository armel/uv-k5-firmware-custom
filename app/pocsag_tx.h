/* POCSAG transmit: encodes a page and clocks it out of the BK4819.
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

#ifndef POCSAG_TX_H
#define POCSAG_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "app/pocsag.h"

// Longest message we will transmit. The encode buffer is sized from this and
// lives in bss, so raising it costs RAM: 40 characters needs 208 bytes.
#define POCSAG_TX_MAX_CHARS      40

// POCSAG is nominally +/-4.5kHz, in the 10Hz units the BK4819 uses.
#define POCSAG_TX_DEVIATION      450

// The modem emits this much preamble and sync ahead of the payload. Sized so
// that with the sync word set to an alternating pattern it forms the first
// part of POCSAG's 576 bit preamble, and the payload carries the rest.
#define POCSAG_TX_HW_PREAMBLE_BYTES   16
#define POCSAG_TX_HW_SYNC_BYTES        4
#define POCSAG_TX_HW_LEAD_BYTES       (POCSAG_TX_HW_PREAMBLE_BYTES + POCSAG_TX_HW_SYNC_BYTES)

// REG_40<11:0> and REG_70<6:0>. Together these set how far the modem swings
// the synthesiser. The defaults are the chip's own; both need calibrating
// against a real +/-4.5kHz on a bench before trusting a pager to decode.
#define POCSAG_TX_HW_DEVIATION   0x4D0
#define POCSAG_TX_HW_GAIN        96

enum POCSAG_TxResult_t {
    POCSAG_TX_OK = 0,
    POCSAG_TX_ERR_TX_NOT_ALLOWED,  // outside the band plan or locked out
    POCSAG_TX_ERR_TOO_LONG,        // message longer than POCSAG_TX_MAX_CHARS
    POCSAG_TX_ERR_ENCODE,
    POCSAG_TX_ERR_MODEM            // the FSK modem rejected the setup or timed out
};
typedef enum POCSAG_TxResult_t POCSAG_TxResult_t;

struct POCSAG_TxStats_t {
    uint32_t bits;             // total on air, preamble included
    uint32_t payload_bytes;    // what went into the FIFO
};
typedef struct POCSAG_TxStats_t POCSAG_TxStats_t;

// Encodes and transmits one page. Blocks for the duration of the burst, which
// is around 1.4s for a 40 character message at 1200 baud.
//
// frequency is in 10Hz units, pa_bias is passed straight to
// BK4819_SetupPowerAmplifier. Keep it low: a pager on the bench needs far less
// than the radio's minimum useful voice power.
//
// stats may be NULL. When given it reports the bit clock accuracy, which is
// the number to look at when a real pager will not decode.
POCSAG_TxResult_t POCSAG_TX_Send(uint32_t frequency,
                                 uint32_t baud,
                                 uint32_t ric,
                                 POCSAG_Function_t func,
                                 POCSAG_MsgType_t type,
                                 const char *msg,
                                 uint8_t pa_bias,
                                 POCSAG_TxStats_t *stats);

#endif
