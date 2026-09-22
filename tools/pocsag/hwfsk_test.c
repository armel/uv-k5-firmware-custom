/* Host tests for driver/bk4819-hwfsk.c and the preamble split.
 *
 * The decisive question for the hardware FSK backend is whether what the modem
 * would put on air is the same bit stream the encoder produced. So this records
 * every register write, reconstructs the on-air stream from those writes the
 * way the chip would (preamble from REG_59/REG_58, sync from REG_5A/5B, data
 * from the REG_5F words) and compares it bit for bit against the encoder.
 *
 * That comparison is what makes the backend trustworthy: roundtrip.sh already
 * proves the encoder's output decodes in multimon-ng, so bit identity carries
 * that result across to the hardware path.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/pocsag.h"
#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d ", __func__, __LINE__);                      \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// --- recorded chip ---------------------------------------------------------

#define MAX_WRITES 4096

static struct { uint8_t reg; uint16_t data; } gW[MAX_WRITES];
static uint32_t gN;

void BK4819_WriteRegister(BK4819_REGISTER_t reg, uint16_t data)
{
    if (gN < MAX_WRITES) {
        gW[gN].reg  = (uint8_t)reg;
        gW[gN].data = data;
    }
    gN++;
}

uint16_t BK4819_ReadRegister(BK4819_REGISTER_t reg)
{
    (void)reg;
    return 1;   // REG_0C bit 0: the packet finished
}

void SYSTEM_DelayMs(uint32_t ms) { (void)ms; }

static void Reset(void) { gN = 0; }

// Last value written to a register, or fallback if it was never written.
static uint16_t Last(uint8_t reg, uint16_t fallback, bool *found)
{
    uint16_t v = fallback;

    if (found) *found = false;

    for (uint32_t i = 0; i < gN && i < MAX_WRITES; i++)
        if (gW[i].reg == reg) {
            v = gW[i].data;
            if (found) *found = true;
        }

    return v;
}

// --- reconstruct what the chip would transmit ------------------------------

struct Bits { uint8_t *b; uint32_t n, cap; };

static void Push(struct Bits *s, bool bit)
{
    if (s->n >= s->cap * 8) { printf("  reconstruction overflow\n"); exit(2); }
    if (bit) s->b[s->n >> 3] |=  (uint8_t)(1u << (7 - (s->n & 7)));
    else     s->b[s->n >> 3] &= (uint8_t)~(1u << (7 - (s->n & 7)));
    s->n++;
}

static void PushByte(struct Bits *s, uint8_t v)
{
    for (int i = 7; i >= 0; i--)
        Push(s, (v >> i) & 1u);
}

// Builds the on-air stream from the recorded writes alone, so the test does
// not get to assume anything the driver did not actually program.
static void Reconstruct(struct Bits *out)
{
    const uint16_t r58 = Last(0x58, 0, NULL);
    const uint16_t r59 = Last(0x59, 0, NULL);
    const uint16_t r5a = Last(0x5A, 0, NULL);
    const uint16_t r5b = Last(0x5B, 0, NULL);

    out->n = 0;

    // REG_58<5:4> preamble type: 11 = 0xAA, 10 = 0x55
    const uint8_t pattern = (((r58 >> 4) & 3u) == 2u) ? 0x55 : 0xAA;

    // REG_59<7:4> preamble length, stored as bytes-1
    const uint32_t preamble = ((r59 >> 4) & 0xFu) + 1u;

    for (uint32_t i = 0; i < preamble; i++)
        PushByte(out, pattern);

    // REG_59<3> selects 2 or 4 sync bytes, sync byte 0 in the high half of REG_5A
    PushByte(out, (uint8_t)(r5a >> 8));
    PushByte(out, (uint8_t)(r5a & 0xFF));

    if (r59 & (1u << 3)) {
        PushByte(out, (uint8_t)(r5b >> 8));
        PushByte(out, (uint8_t)(r5b & 0xFF));
    }

    /* The FIFO words, low byte first, because that is the order the hardware
     * actually transmits them in. This used to model high byte first, matching
     * the assumption in the driver, so the test agreed with the code and both
     * were wrong: on air every byte pair came out swapped. A synthetic test
     * that shares the code's misreading cannot catch it, which is precisely
     * why this had to be measured off air. */
    for (uint32_t i = 0; i < gN && i < MAX_WRITES; i++)
        if (gW[i].reg == 0x5F) {
            PushByte(out, (uint8_t)(gW[i].data & 0xFF));
            PushByte(out, (uint8_t)(gW[i].data >> 8));
        }
}

// --- tests -----------------------------------------------------------------

#define LEAD_BYTES 20   // 16 byte preamble + 4 byte sync

static void Configure(BK4819_HwFskConfig_t *cfg, uint32_t baud)
{
    cfg->baud           = baud;
    cfg->preamble_bytes = 16;
    cfg->sync_bytes     = 4;
    cfg->sync01         = 0xAAAA;
    cfg->sync23         = 0xAAAA;
    cfg->deviation      = 0x4D0;
    cfg->gain           = 96;
    cfg->invert         = false;
    cfg->crc            = false;
    cfg->scramble       = false;
}

static void TestBaudWord(void)
{
    // 1200 baud must land on 0x3065, the constant the stock aircopy code uses.
    // That is the check that the datasheet formula was read correctly.
    const struct { uint32_t baud; uint16_t word; } v[] = {
        { 512, 0x14A6 }, { 1200, 0x3065 }, { 2400, 0x60CB },
    };

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        const uint16_t got = BK4819_HwFskBaudWord(v[i].baud);
        CHECK(got == v[i].word, "%u baud gave 0x%04X, expected 0x%04X",
              v[i].baud, got, v[i].word);
    }

    // Past about 6347 baud the control word overflows the register.
    BK4819_HwFskConfig_t cfg;
    Configure(&cfg, 20000);
    CHECK(BK4819_HwFskCheck(&cfg, 64) == BK4819_HWFSK_ERR_BAUD,
          "20000 baud should overflow REG_72 and be refused");
}

static void TestValidation(void)
{
    BK4819_HwFskConfig_t cfg;

    Configure(&cfg, 1200);
    CHECK(BK4819_HwFskCheck(&cfg, 64) == BK4819_HWFSK_OK, "valid config refused");

    cfg.preamble_bytes = 17;
    CHECK(BK4819_HwFskCheck(&cfg, 64) == BK4819_HWFSK_ERR_PREAMBLE, "17 byte preamble allowed");
    cfg.preamble_bytes = 0;
    CHECK(BK4819_HwFskCheck(&cfg, 64) == BK4819_HWFSK_ERR_PREAMBLE, "0 byte preamble allowed");

    Configure(&cfg, 1200);
    cfg.sync_bytes = 3;
    CHECK(BK4819_HwFskCheck(&cfg, 64) == BK4819_HWFSK_ERR_SYNC_LEN, "3 byte sync allowed");

    Configure(&cfg, 1200);
    CHECK(BK4819_HwFskCheck(&cfg, 0)   == BK4819_HWFSK_ERR_LENGTH, "empty payload allowed");
    CHECK(BK4819_HwFskCheck(&cfg, 65)  == BK4819_HWFSK_ERR_LENGTH, "odd payload allowed");
    CHECK(BK4819_HwFskCheck(&cfg, 257) == BK4819_HWFSK_ERR_LENGTH, "payload past one FIFO allowed");
    CHECK(BK4819_HwFskCheck(&cfg, 256) == BK4819_HWFSK_OK, "a full FIFO should be fine");
}

static void TestSplit(void)
{
    static uint8_t  storage[512];
    POCSAG_BitBuf_t buf;
    uint32_t        bytes = 0;

    POCSAG_BufInit(&buf, storage, sizeof(storage));
    POCSAG_Encode(&buf, 1234567, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "SPLIT");

    const uint8_t *p = POCSAG_SplitPreamble(buf.data, buf.length, LEAD_BYTES, &bytes);

    CHECK(p == buf.data + LEAD_BYTES, "split did not land past the lead bytes");
    CHECK(bytes == (buf.length / 8) - LEAD_BYTES, "payload length wrong");

    // The lead the modem replaces has to be real alternating preamble.
    for (uint32_t i = 0; i < LEAD_BYTES; i++)
        CHECK(buf.data[i] == 0xAA, "lead byte %u is %02X, not 0xAA", i, buf.data[i]);

    // Refuse a stream whose lead is not preamble, rather than splicing garbage.
    static uint8_t bad[128];
    memset(bad, 0xAA, sizeof(bad));
    bad[7] = 0x00;
    CHECK(POCSAG_SplitPreamble(bad, sizeof(bad) * 8, LEAD_BYTES, &bytes) == NULL,
          "split accepted a non-preamble lead");

    // Too short, and misaligned, must also be refused.
    CHECK(POCSAG_SplitPreamble(bad, LEAD_BYTES * 8, LEAD_BYTES, &bytes) == NULL,
          "split accepted a stream with no payload left");
    CHECK(POCSAG_SplitPreamble(bad, (LEAD_BYTES * 8) + 4, LEAD_BYTES, &bytes) == NULL,
          "split accepted a stream that is not byte aligned");
}

// The central test: what the chip would transmit must equal what the encoder
// produced, bit for bit.
static void TestOnAirMatchesEncoder(uint32_t ric, uint32_t baud,
                                    POCSAG_MsgType_t type, const char *msg)
{
    static uint8_t       storage[512];
    static uint8_t       air[512];
    POCSAG_BitBuf_t      buf;
    BK4819_HwFskConfig_t cfg;
    struct Bits          out = { air, 0, sizeof(air) };
    uint32_t             bytes = 0;

    POCSAG_BufInit(&buf, storage, sizeof(storage));

    if (!POCSAG_Encode(&buf, ric, POCSAG_FUNC_D, type, msg)) {
        CHECK(false, "encode failed for ric %u", ric);
        return;
    }

    const uint8_t *payload = POCSAG_SplitPreamble(buf.data, buf.length, LEAD_BYTES, &bytes);

    if (payload == NULL) { CHECK(false, "split failed for ric %u", ric); return; }

    Configure(&cfg, baud);
    Reset();

    CHECK(BK4819_HwFskSetup(&cfg) == BK4819_HWFSK_OK, "setup failed");
    CHECK(BK4819_HwFskSend(&cfg, payload, bytes) == BK4819_HWFSK_OK, "send failed");

    Reconstruct(&out);

    CHECK(out.n == buf.length,
          "ric %u %u baud: on air %u bits, encoder made %u", ric, baud, out.n, buf.length);

    if (out.n == buf.length && memcmp(air, buf.data, buf.length / 8) != 0) {
        uint32_t first = 0;
        while (first < buf.length / 8 && air[first] == buf.data[first])
            first++;
        CHECK(false, "ric %u %u baud: differs from byte %u (%02X vs %02X)",
              ric, baud, first, air[first], buf.data[first]);
    }

    // The programmed rate and length have to match too.
    CHECK(Last(0x72, 0, NULL) == BK4819_HwFskBaudWord(baud), "REG_72 wrong for %u baud", baud);

    const uint16_t r5d = Last(0x5D, 0, NULL);
    const uint32_t len = (((r5d >> 8) & 0xFF) | (((r5d >> 5) & 0x7) << 8)) + 1u;
    CHECK(len == bytes, "REG_5D says %u bytes, payload is %u", len, bytes);

    // CRC and scrambling must be off, or the pager sees trailing junk.
    CHECK((Last(0x5C, 0, NULL) & (1u << 6)) == 0, "CRC left enabled");
    CHECK((Last(0x59, 0, NULL) & (1u << 13)) == 0, "scramble left enabled");
}

static void TestOnAir(void)
{
    const uint32_t bauds[] = { 512, 1200, 2400 };

    for (size_t b = 0; b < 3; b++) {
        // every frame position, so the idle padding varies
        for (uint32_t i = 0; i < 8; i++)
            TestOnAirMatchesEncoder(1000000 + i, bauds[b], POCSAG_MSG_ALPHA, "FRAME");

        TestOnAirMatchesEncoder(1234567, bauds[b], POCSAG_MSG_TONE, "");
        TestOnAirMatchesEncoder(1234567, bauds[b], POCSAG_MSG_NUMERIC, "0123456789");
        TestOnAirMatchesEncoder(2097151, bauds[b], POCSAG_MSG_ALPHA, "EDGE RIC");
        TestOnAirMatchesEncoder(0,       bauds[b], POCSAG_MSG_ALPHA, "ZERO");
    }

    // message lengths that straddle a batch boundary
    for (uint32_t n = 1; n <= 40; n++) {
        char msg[64];
        memset(msg, 'A', n);
        msg[n] = '\0';
        TestOnAirMatchesEncoder(1234567, 1200, POCSAG_MSG_ALPHA, msg);
    }
}

static void TestFifoCapacity(void)
{
    static uint8_t  storage[1024];
    POCSAG_BitBuf_t buf;
    uint32_t        bytes = 0;
    char            msg[64];

    // The 40 character cap must stay inside a single FIFO load, since the
    // driver does not refill mid packet.
    memset(msg, 'W', 40);
    msg[40] = '\0';

    POCSAG_BufInit(&buf, storage, sizeof(storage));
    POCSAG_Encode(&buf, 1234567, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, msg);
    POCSAG_SplitPreamble(buf.data, buf.length, LEAD_BYTES, &bytes);

    CHECK(bytes <= BK4819_HWFSK_MAX_PAYLOAD,
          "a 40 char page needs %u bytes, FIFO holds %u", bytes, BK4819_HWFSK_MAX_PAYLOAD);

    printf("  fifo: worst case 40 char page is %u of %u payload bytes (%u words)\n",
           bytes, BK4819_HWFSK_MAX_PAYLOAD, bytes / 2);
}

int main(void)
{
    printf("bk4819 hardware FSK host tests\n");

    TestBaudWord();
    TestValidation();
    TestSplit();
    TestOnAir();
    TestFifoCapacity();

    printf("%s\n", failures ? "FAILED" : "all passed");

    return failures ? 1 : 0;
}
