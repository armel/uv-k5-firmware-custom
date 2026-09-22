/* POCSAG transmit: encodes a page and sends it through the BK4819 FSK modem.
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
#include <string.h>

#include "app/pocsag_tx.h"

#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "frequencies.h"
#include "radio.h"

#define POCSAG_TX_BUF_BYTES   POCSAG_ALPHA_BUF_BYTES(POCSAG_TX_MAX_CHARS)

static uint8_t gPocsagTxBuffer[POCSAG_TX_BUF_BYTES];

static void HwConfig(BK4819_HwFskConfig_t *cfg, uint32_t baud)
{
    cfg->baud           = baud;
    cfg->preamble_bytes = POCSAG_TX_HW_PREAMBLE_BYTES;
    cfg->sync_bytes     = POCSAG_TX_HW_SYNC_BYTES;

    // Not a sync word in any real sense, just four more bytes of the
    // alternating preamble the pager is waiting for.
    cfg->sync01         = 0xAAAA;
    cfg->sync23         = 0xAAAA;

    cfg->deviation      = POCSAG_TX_HW_DEVIATION;
    cfg->gain           = POCSAG_TX_HW_GAIN;
    cfg->invert         = false;
    cfg->crc            = false;   // POCSAG carries its own BCH
    cfg->scramble       = false;
}

/* Keying. This is app-level orchestration of driver calls, in the same way
 * RADIO_SetTxParameters is, and it mirrors that sequence because that is the
 * one known to work on this hardware.
 *
 * The RX_ENABLE line matters: clearing it is what routes the PA to the antenna,
 * and omitting it produced no RF at all on the first bench test while the PA
 * ran happily. PrepareTransmit rather than EnableTXLink, because the latter
 * sets REG_30 but skips REG_37 and REG_52. The ordering and the delays are
 * taken from RADIO_SetTxParameters rather than invented. */
static void TxOn(uint32_t frequency, uint8_t pa_bias)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

    BK4819_WriteRegister(BK4819_REG_70, 0);   // no tone generators
    BK4819_ExitSubAu();                       // no CTCSS/CDCSS
    BK4819_SetCompander(0);

    BK4819_SetFrequency(frequency);

    BK4819_PrepareTransmit();
    SYSTEM_DelayMs(10);

    BK4819_PickRXFilterPathBasedOnFrequency(frequency);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTEM_DelayMs(5);

    BK4819_SetupPowerAmplifier(pa_bias, frequency);
    SYSTEM_DelayMs(10);
}

static void TxOff(void)
{
    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_30, 0);

    // put the transmit/receive switch back so the radio can hear again
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
}

static void Stop(void)
{
    BK4819_HwFskStop();
    TxOff();
    RADIO_SetupRegisters(true);
}

/* Programs the modem and sends one payload.
 *
 * Keying happens BEFORE the modem is programmed, not after: TxOn clears REG_70
 * to silence the tone generators, and the low 7 bits of REG_70 are the
 * TONE2/FSK tuning gain, which is the modem's deviation. Doing it the other way
 * round put a clean unmodulated carrier on air, measured at +/-330 Hz where
 * +/-4500 was wanted.
 *
 * The audio path is deliberately left unmuted here, unlike a plain carrier: the
 * modem feeds the modulator through it, which is why aircopy also transmits
 * unmuted. */
static POCSAG_TxResult_t SendPayload(uint32_t frequency, uint32_t baud,
                                     const uint8_t *payload, uint32_t bytes,
                                     uint8_t pa_bias, POCSAG_TxStats_t *stats)
{
    BK4819_HwFskConfig_t cfg;

    HwConfig(&cfg, baud);

    if (BK4819_HwFskCheck(&cfg, bytes) != BK4819_HWFSK_OK)
        return POCSAG_TX_ERR_MODEM;

    TxOn(frequency, pa_bias);

    if (BK4819_HwFskSetup(&cfg) != BK4819_HWFSK_OK) {
        Stop();
        return POCSAG_TX_ERR_MODEM;
    }

    const BK4819_HwFskStatus_t st = BK4819_HwFskSend(&cfg, payload, bytes);

    Stop();

    if (stats != NULL) {
        stats->bits          = (bytes + POCSAG_TX_HW_LEAD_BYTES) * 8u;
        stats->payload_bytes = bytes;
    }

    return (st == BK4819_HWFSK_OK) ? POCSAG_TX_OK : POCSAG_TX_ERR_MODEM;
}

POCSAG_TxResult_t POCSAG_TX_Send(uint32_t frequency,
                                 uint32_t baud,
                                 uint32_t ric,
                                 POCSAG_Function_t func,
                                 POCSAG_MsgType_t type,
                                 const char *msg,
                                 uint8_t pa_bias,
                                 POCSAG_TxStats_t *stats)
{
    const uint32_t len = (msg != NULL) ? (uint32_t)strlen(msg) : 0;

    if (len > POCSAG_TX_MAX_CHARS)
        return POCSAG_TX_ERR_TOO_LONG;

    // The static buffer is sized by a macro, so confirm against the encoder's
    // own arithmetic rather than assuming the two agree.
    if (POCSAG_EstimateBytes(type, len) > sizeof(gPocsagTxBuffer))
        return POCSAG_TX_ERR_TOO_LONG;

    if (TX_freq_check(frequency) != 0)
        return POCSAG_TX_ERR_TX_NOT_ALLOWED;

    POCSAG_BitBuf_t buf;
    POCSAG_BufInit(&buf, gPocsagTxBuffer, sizeof(gPocsagTxBuffer));

    if (!POCSAG_Encode(&buf, ric, func, type, msg))
        return POCSAG_TX_ERR_ENCODE;

    uint32_t bytes = 0;
    const uint8_t *payload = POCSAG_SplitPreamble(buf.data, buf.length,
                                                 POCSAG_TX_HW_LEAD_BYTES, &bytes);

    if (payload == NULL)
        return POCSAG_TX_ERR_ENCODE;

    return SendPayload(frequency, baud, payload, bytes, pa_bias, stats);
}
