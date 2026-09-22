/* BK4819 hardware FSK modem. See bk4819-hwfsk.h.
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

#include <stdint.h>

#ifdef BK4819_FSK_HOST_TEST
    void SYSTEM_DelayMs(uint32_t ms);   // the host test provides its own
#else
    #include "system.h"
#endif

#include "bk4819-hwfsk.h"
#include "bk4819.h"

// REG_5C holds bits whose meaning is not documented anywhere we have. The
// stock firmware writes 0x5665, so start from that and only touch the CRC bit
// rather than guessing at the rest.
#define REG_5C_BASE        0x5665u
#define REG_5C_CRC_ENABLE  (1u << 6)

// REG_58, assembled from the register list:
//   <15:13> Tx mode, 000 = FSK 1.2K / 2.4K
//   <12:10> Rx mode, 000
//   <9:8>   Rx gain
//   <5:4>   preamble type, 11 = 0xAA
//   <3:1>   Rx bandwidth, 000 = FSK 1.2K
//   <0>     enable
//
// Note the stock aircopy code sets <7:6> instead of <5:4> for the preamble
// type. Two independent documents put it at <5:4>, so that is what we use, but
// if a bench test shows the preamble coming out as 0x55 this is the first
// thing to try moving.
#define REG_58_FSK_TX      ((0u << 13) | (0u << 10) | (0u << 8) | (0x3u << 4) | (0u << 1) | 1u)

#define REG_59_CLEAR_TX    (1u << 15)
#define REG_59_CLEAR_RX    (1u << 14)
#define REG_59_SCRAMBLE    (1u << 13)
#define REG_59_RX_ENABLE   (1u << 12)
#define REG_59_TX_ENABLE   (1u << 11)
#define REG_59_RX_INVERT   (1u << 10)
#define REG_59_TX_INVERT   (1u <<  9)
#define REG_59_SYNC_4BYTE  (1u <<  3)

// REG_0B, read only
#define REG_0B_SYNC_NEGATIVE  (1u << 7)
#define REG_0B_SYNC_POSITIVE  (1u << 6)

// REG_5E: TX almost-empty threshold in <9:3>, RX almost-full in <2:0>. Written
// explicitly rather than trusting the reset value, because the RX poll reads a
// fixed number of words per interrupt and so depends on this being exact.
#define REG_5E_THRESHOLDS  ((64u << 3) | BK4819_HWFSK_RX_THRESHOLD)

// Full precision, so the caller can tell an out of range rate from one that
// merely wrapped when truncated to the 16 bit register.
static uint32_t BaudWordRaw(uint32_t baud)
{
    // 10.32444 * 2^17 = 1353247, with 2^16 added to round rather than truncate
    return (uint32_t)(((uint64_t)baud * 1353247u + (1u << 16)) >> 17);
}

uint16_t BK4819_HwFskBaudWord(uint32_t baud)
{
    return (uint16_t)BaudWordRaw(baud);
}

BK4819_HwFskStatus_t BK4819_HwFskCheck(const BK4819_HwFskConfig_t *cfg, uint32_t len)
{
    const uint32_t word = BaudWordRaw(cfg->baud);

    // Above about 6347 baud the control word no longer fits the register.
    if (cfg->baud == 0 || word == 0 || word > 0xFFFF)
        return BK4819_HWFSK_ERR_BAUD;

    if (cfg->preamble_bytes < 1 || cfg->preamble_bytes > BK4819_HWFSK_MAX_PREAMBLE)
        return BK4819_HWFSK_ERR_PREAMBLE;

    if (cfg->sync_bytes != 2 && cfg->sync_bytes != 4)
        return BK4819_HWFSK_ERR_SYNC_LEN;

    // The FIFO is written a word at a time and we do not refill it mid packet,
    // so the payload has to be even and fit in one load.
    if (len == 0 || (len & 1) != 0 || len > BK4819_HWFSK_MAX_PAYLOAD)
        return BK4819_HWFSK_ERR_LENGTH;

    return BK4819_HWFSK_OK;
}

static uint16_t Reg59Config(const BK4819_HwFskConfig_t *cfg)
{
    uint16_t v = (uint16_t)((cfg->preamble_bytes - 1) << 4);

    if (cfg->sync_bytes == 4) v |= REG_59_SYNC_4BYTE;
    if (cfg->scramble)        v |= REG_59_SCRAMBLE;
    if (cfg->invert)          v |= REG_59_TX_INVERT;

    return v;
}

BK4819_HwFskStatus_t BK4819_HwFskSetup(const BK4819_HwFskConfig_t *cfg)
{
    // len is not known here, so check everything except it
    const BK4819_HwFskStatus_t st = BK4819_HwFskCheck(cfg, 2);

    if (st != BK4819_HWFSK_OK)
        return st;

    BK4819_WriteRegister(BK4819_REG_58, REG_58_FSK_TX);
    BK4819_WriteRegister(BK4819_REG_72, BK4819_HwFskBaudWord(cfg->baud));

    BK4819_WriteRegister(BK4819_REG_5A, cfg->sync01);
    BK4819_WriteRegister(BK4819_REG_5B, cfg->sync23);

    BK4819_WriteRegister(BK4819_REG_5C,
        cfg->crc ? (REG_5C_BASE | REG_5C_CRC_ENABLE)
                 : (REG_5C_BASE & ~REG_5C_CRC_ENABLE));

    // REG_40<12> enables the deviation tuning in <11:0>. The upper bits are
    // undocumented and left clear.
    BK4819_WriteRegister(BK4819_REG_40, (1u << 12) | (cfg->deviation & 0x0FFF));

    // The FSK modulator shares its gain field with TONE2, and the stock code
    // enables the TONE2 bit alongside it.
    BK4819_WriteRegister(BK4819_REG_70, (1u << 7) | (cfg->gain & 0x7F));

    BK4819_WriteRegister(BK4819_REG_59, REG_59_CLEAR_TX | REG_59_CLEAR_RX | Reg59Config(cfg));
    BK4819_WriteRegister(BK4819_REG_59, Reg59Config(cfg));

    return BK4819_HWFSK_OK;
}

BK4819_HwFskStatus_t BK4819_HwFskSend(const BK4819_HwFskConfig_t *cfg,
                                      const uint8_t *payload,
                                      uint32_t len)
{
    const BK4819_HwFskStatus_t st = BK4819_HwFskCheck(cfg, len);

    if (st != BK4819_HWFSK_OK)
        return st;

    /* REG_5D holds length-1, split across two fields: the datasheet gives 0xF
     * as meaning 16 bytes and the stock aircopy code writes 0x47 for a 72 byte
     * packet, and a page sent this way decoded correctly off air. */
    const uint32_t encoded = len - 1;

    BK4819_WriteRegister(BK4819_REG_5D,
        (uint16_t)(((encoded & 0xFF) << 8) | (((encoded >> 8) & 0x7) << 5)));

    BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_TX_FINISHED);

    BK4819_WriteRegister(BK4819_REG_59, REG_59_CLEAR_TX | Reg59Config(cfg));
    BK4819_WriteRegister(BK4819_REG_59, Reg59Config(cfg));

    /* The FIFO transmits each 16 bit word LOW byte first, so the earlier byte of
     * the pair belongs in the low half. Packing it the other way round, which
     * the sync byte order in REG_5A had suggested, swapped every byte pair on
     * air: an off-air capture of a tone page decoded to 607F0789 and 7A97C189
     * where 605A077F and 7A89C197 were sent, which is exactly this swap seen
     * through a one byte window offset. */
    for (uint32_t i = 0; i < len; i += 2)
        BK4819_WriteRegister(BK4819_REG_5F,
            (uint16_t)((payload[i + 1] << 8) | payload[i]));

    BK4819_WriteRegister(BK4819_REG_59, REG_59_TX_ENABLE | Reg59Config(cfg));

    // Worst case is 2047 bytes at 512 baud, about 32 seconds. Poll well past
    // that rather than risk cutting a packet short.
    bool finished = false;

    for (uint32_t i = 0; i < 8000 && !finished; i++) {
        if (BK4819_ReadRegister(BK4819_REG_0C) & 1u)
            finished = true;
        else
            SYSTEM_DelayMs(5);
    }

    BK4819_WriteRegister(BK4819_REG_02, 0);

    return finished ? BK4819_HWFSK_OK : BK4819_HWFSK_ERR_TIMEOUT;
}

void BK4819_HwFskStop(void)
{
    BK4819_WriteRegister(BK4819_REG_59, REG_59_CLEAR_TX | REG_59_CLEAR_RX);
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_WriteRegister(BK4819_REG_58, 0);
    BK4819_WriteRegister(BK4819_REG_3F, 0);
}

// --- receive ---------------------------------------------------------------

/* REG_58 for receive. The low bits are the stock aircopy value, which is the only
 * FSK receive configuration known to work on this hardware, with the RX gain left
 * at its default:
 *
 *   <15:13> 000 Tx mode, unused here
 *   <12:10> 000 Rx mode, FSK 1.2K / 2.4K
 *   <9:8>   00  Rx gain
 *   <7:6>   11  not in the register list at all; the stock code sets it as
 *               though it were the preamble type and does receive, so it stays
 *   <5:4>   11  the documented preamble type field, 11 = 0xAA
 *   <3:1>   000 Rx bandwidth, FSK 1.2K
 *   <0>     1   enable
 *
 * Both candidate preamble type fields are set to 11, deliberately. The register
 * list puts the field at <5:4>, the stock code behaves as though it were at
 * <7:6>, and setting both says "the preamble is 0xAA" whichever is right.
 *
 * It matters because the alternative, <5:4>=00, means "0xAA or 0x55 decided by
 * the MSB of sync byte 0". POCSAG's sync byte 0 is 0x7C, whose MSB is 0, so that
 * setting could have the chip expect the 0x55 phase - a byte boundary one bit out
 * from the real one, which would leave the sync word never matching however good
 * the signal was. The preamble on air really is 0xAA aligned: the transmit path
 * sets 0xAA and a pager decodes it, and POCSAG's 576 preamble bits put the sync
 * word on an even bit boundary. So say so rather than leave it to be inferred. */
#define REG_58_FSK_RX      0x00F1u

/* <3:1> is the receive bandwidth, and it has to follow the bit rate: the register
 * list gives 000 "for FSK 1.2K" and 100 "for FSK 2.4K", and REG_58_FSK_RX above
 * carries 000.
 *
 * Leaving it at 000 regardless of rate is wrong above 1200 baud, and measurably so.
 * The APRS work found a 4800 baud stream would not lock at all through the narrow
 * setting and that 2400 worked only just; widening it fixed both. POCSAG offers 2400
 * baud, so it was running with that reduced margin too.
 */
static uint16_t Reg58Rx(uint32_t baud)
{
    return (uint16_t)(REG_58_FSK_RX | ((baud > 1200u) ? (4u << 1) : 0u));
}

static uint16_t Reg59RxConfig(const BK4819_HwFskConfig_t *cfg)
{
    uint16_t v = (uint16_t)((cfg->preamble_bytes - 1) << 4);

    if (cfg->sync_bytes == 4) v |= REG_59_SYNC_4BYTE;
    if (cfg->scramble)        v |= REG_59_SCRAMBLE;
    if (cfg->invert)          v |= REG_59_RX_INVERT;

    return v;
}

BK4819_HwFskStatus_t BK4819_HwFskRxCheck(const BK4819_HwFskConfig_t *cfg, uint32_t data_bytes)
{
    const uint32_t word = BaudWordRaw(cfg->baud);

    if (cfg->baud == 0 || word == 0 || word > 0xFFFF)
        return BK4819_HWFSK_ERR_BAUD;

    if (cfg->preamble_bytes < 1 || cfg->preamble_bytes > BK4819_HWFSK_MAX_PREAMBLE)
        return BK4819_HWFSK_ERR_PREAMBLE;

    if (cfg->sync_bytes != 2 && cfg->sync_bytes != 4)
        return BK4819_HWFSK_ERR_SYNC_LEN;

    // Whole chunks only: the almost-full interrupt is the only measure of how
    // much the FIFO holds, so a remainder would be stranded there.
    if (data_bytes == 0 ||
        data_bytes > BK4819_HWFSK_RX_MAX_LENGTH ||
        (data_bytes % BK4819_HWFSK_RX_CHUNK) != 0)
        return BK4819_HWFSK_ERR_LENGTH;

    return BK4819_HWFSK_OK;
}

BK4819_HwFskStatus_t BK4819_HwFskRxSetup(const BK4819_HwFskConfig_t *cfg, uint32_t data_bytes)
{
    const BK4819_HwFskStatus_t st = BK4819_HwFskRxCheck(cfg, data_bytes);

    if (st != BK4819_HWFSK_OK)
        return st;

    BK4819_WriteRegister(BK4819_REG_58, Reg58Rx(cfg->baud));
    BK4819_WriteRegister(BK4819_REG_72, BK4819_HwFskBaudWord(cfg->baud));

    BK4819_WriteRegister(BK4819_REG_5A, cfg->sync01);
    BK4819_WriteRegister(BK4819_REG_5B, cfg->sync23);

    BK4819_WriteRegister(BK4819_REG_5C,
        cfg->crc ? (REG_5C_BASE | REG_5C_CRC_ENABLE)
                 : (REG_5C_BASE & ~REG_5C_CRC_ENABLE));

    // As on transmit, REG_5D holds length-1 split across two fields.
    const uint32_t encoded = data_bytes - 1;

    BK4819_WriteRegister(BK4819_REG_5D,
        (uint16_t)(((encoded & 0xFF) << 8) | (((encoded >> 8) & 0x7) << 5)));

    BK4819_WriteRegister(BK4819_REG_5E, REG_5E_THRESHOLDS);

    BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_RX_SYNC |
                                        BK4819_REG_3F_FSK_FIFO_ALMOST_FULL |
                                        BK4819_REG_3F_FSK_RX_FINISHED);

    BK4819_HwFskRxRestart(cfg);

    return BK4819_HWFSK_OK;
}

void BK4819_HwFskRxRestart(const BK4819_HwFskConfig_t *cfg)
{
    const uint16_t base = Reg59RxConfig(cfg);

    BK4819_WriteRegister(BK4819_REG_59, REG_59_CLEAR_RX | base);
    BK4819_WriteRegister(BK4819_REG_59, REG_59_RX_ENABLE | base);

    // Any stale interrupt would otherwise be read as belonging to the packet
    // that is about to arrive.
    BK4819_WriteRegister(BK4819_REG_02, 0);
}

uint8_t BK4819_HwFskRxPoll(uint8_t *dst, uint8_t max, BK4819_HwFskRxEvent_t *ev)
{
    uint8_t n = 0;

    ev->sync          = false;
    ev->sync_inverted = false;
    ev->finished      = false;
    ev->overrun       = false;

    // Bounded, unlike the equivalent loop in app.c: a chip that held its
    // interrupt line asserted would otherwise hang the receive screen.
    for (uint8_t pass = 0; pass < 8; pass++) {
        if ((BK4819_ReadRegister(BK4819_REG_0C) & 1u) == 0)
            break;

        BK4819_WriteRegister(BK4819_REG_02, 0);

        const uint16_t flags = BK4819_ReadRegister(BK4819_REG_02);

        if (flags & BK4819_REG_3F_FSK_RX_SYNC) {
            const uint16_t status = BK4819_ReadRegister(BK4819_REG_0B);

            ev->sync = true;

            // The chip matches the sync word in both senses and says which one
            // it found, so the data polarity comes for free and does not have
            // to be configured or guessed.
            ev->sync_inverted = (status & REG_0B_SYNC_NEGATIVE) != 0 &&
                                (status & REG_0B_SYNC_POSITIVE) == 0;
        }

        if (flags & BK4819_REG_3F_FSK_FIFO_ALMOST_FULL) {
            if ((uint32_t)(max - n) < BK4819_HWFSK_RX_CHUNK) {
                // Leave it in the FIFO. The threshold is still met, so the
                // interrupt comes straight back on the next poll.
                ev->overrun = true;
                break;
            }

            for (uint8_t i = 0; i < BK4819_HWFSK_RX_THRESHOLD; i++) {
                const uint16_t w = BK4819_ReadRegister(BK4819_REG_5F);

                // Low byte first, the same order the TX FIFO uses.
                dst[n++] = (uint8_t)(w & 0xFFu);
                dst[n++] = (uint8_t)(w >> 8);
            }
        }

        if (flags & BK4819_REG_3F_FSK_RX_FINISHED)
            ev->finished = true;
    }

    return n;
}
