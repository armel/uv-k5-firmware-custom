/* Host tests for the POCSAG receive decoder.
 *
 * The encoder and the decoder are the two halves of the same format, so the
 * test drives one with the other: encode a page, hand the decoder the bit
 * stream from the point where the BK4819's FSK receiver would hand over, and
 * check that what comes out is what went in. roundtrip.sh already proves the
 * encoder's output is real POCSAG by decoding it with multimon-ng, so agreement
 * here means the decoder reads real POCSAG too.
 *
 * Beyond the happy path it checks what only matters on the air: the frame the
 * address arrived in being part of the address, single bit error correction,
 * getting back in step after a corrupted sync word, inverted data, and that
 * noise decodes as nothing at all.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/pocsag.h"
#include "app/pocsag_rx.h"

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

#define MAX_CHARS 120

static uint8_t         gBuf[POCSAG_ALPHA_BUF_BYTES(MAX_CHARS)];
static POCSAG_BitBuf_t gBits;

// Everything after the preamble and the sync word, which is what the modem
// delivers: it swallows both and starts the FIFO at the first data bit.
#define LEAD_BYTES ((POCSAG_PREAMBLE_BITS / 8u) + 4u)

static uint32_t Encode(uint32_t ric, POCSAG_Function_t func,
                       POCSAG_MsgType_t type, const char *msg)
{
    POCSAG_BufInit(&gBits, gBuf, sizeof(gBuf));

    if (!POCSAG_Encode(&gBits, ric, func, type, msg))
        return 0;

    return (gBits.length / 8u) - LEAD_BYTES;
}

static const uint8_t *Data(void) { return gBuf + LEAD_BYTES; }

static void Invert(uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)~p[i];
}

static void FlipBit(uint8_t *p, uint32_t bit)
{
    p[bit >> 3] ^= (uint8_t)(1u << (7 - (bit & 7)));
}

/* Feeds in the same 8 byte chunks the RX FIFO delivers, rather than one big
 * buffer, so that anything depending on where a codeword straddles a call would
 * show up. */
static POCSAG_RxEvent_t FeedChunked(POCSAG_Rx_t *rx, const uint8_t *data, uint32_t len)
{
    POCSAG_RxEvent_t best = POCSAG_RX_EVENT_NONE;

    for (uint32_t off = 0; off < len; off += 8) {
        const uint32_t n  = (len - off < 8) ? (len - off) : 8;
        const POCSAG_RxEvent_t ev = POCSAG_RxFeed(rx, data + off, n);

        if (ev == POCSAG_RX_EVENT_PAGE || best == POCSAG_RX_EVENT_NONE)
            best = ev;
    }

    return best;
}

// One page in, one page out. Returns the event, with the decoder left to inspect.
static POCSAG_RxEvent_t Roundtrip(POCSAG_Rx_t *rx, uint32_t tx_ric, uint32_t rx_ric,
                                  POCSAG_Function_t func, POCSAG_MsgType_t type,
                                  const char *msg, bool invert)
{
    const uint32_t len = Encode(tx_ric, func, type, msg);

    if (len == 0)
        return POCSAG_RX_EVENT_NONE;

    if (invert)
        Invert(gBuf, gBits.length / 8u);

    POCSAG_RxInit(rx, rx_ric, false);
    POCSAG_RxResync(rx, invert);

    return FeedChunked(rx, Data(), len);
}

// --- tests -----------------------------------------------------------------

static void TestAlpha(void)
{
    POCSAG_Rx_t rx;
    const uint32_t ric = 1578624;

    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, ric, ric, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "HELLO", false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page, event %d", ev);
    CHECK(rx.page_ric == ric, "ric %u, wanted %u", rx.page_ric, ric);
    CHECK(rx.page_func == POCSAG_FUNC_D, "func %u", rx.page_func);
    CHECK(strcmp(rx.msg, "HELLO") == 0, "msg \"%s\"", rx.msg);
    CHECK(!rx.truncated, "reported truncated");
    CHECK(rx.bad == 0, "%u codewords failed BCH on a clean stream", rx.bad);
    CHECK(rx.corrected == 0, "%u codewords corrected on a clean stream", rx.corrected);
}

static void TestPrintableRange(void)
{
    POCSAG_Rx_t rx;
    char msg[POCSAG_RX_MAX_CHARS + 1];
    uint8_t n = 0;

    // Every printable character the format carries, as far as the decoder will
    // hold, so that nothing in the 7 bit packing is lost or shifted.
    for (char c = ' '; c < 0x7F && n < POCSAG_RX_MAX_CHARS; c++)
        msg[n++] = c;
    msg[n] = '\0';

    // The encoder trims nothing, but the decoder drops trailing blanks, and the
    // string starts with one, so compare against a trimmed copy.
    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 100000, 100000, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, msg, false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page");
    CHECK(strcmp(rx.msg, msg) == 0, "\"%s\" came back as \"%s\"", msg, rx.msg);
}

static void TestNumeric(void)
{
    POCSAG_Rx_t rx;

    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 42, 42, POCSAG_FUNC_A, POCSAG_MSG_NUMERIC, "0123456789", false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page");
    // Numeric pages are padded to the codeword with the space code, which the
    // decoder trims, so the digits should come back exactly.
    CHECK(strcmp(rx.msg, "0123456789") == 0, "msg \"%s\"", rx.msg);
}

static void TestTone(void)
{
    POCSAG_Rx_t rx;

    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 999, 999, POCSAG_FUNC_A, POCSAG_MSG_TONE, NULL, false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "a tone only page should still be a page");
    CHECK(rx.msg_len == 0, "tone page carried \"%s\"", rx.msg);
    CHECK(rx.page_ric == 999, "ric %u", rx.page_ric);
}

static void TestEveryFrame(void)
{
    // The three low bits of the address are not sent; they are the frame the
    // address codeword sits in. Get that wrong and seven addresses out of eight
    // decode as something else, so walk all eight.
    for (uint32_t f = 0; f < 8; f++) {
        POCSAG_Rx_t rx;
        const uint32_t ric = 1578620 + f - (1578620 & 7) + (f & 7);

        const POCSAG_RxEvent_t ev =
            Roundtrip(&rx, ric, ric, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "FRAME", false);

        CHECK(ev == POCSAG_RX_EVENT_PAGE, "frame %u: no page for ric %u", f, ric);
        CHECK(rx.page_ric == ric, "frame %u: ric %u, wanted %u", f, rx.page_ric, ric);
    }
}

static void TestWrongRic(void)
{
    POCSAG_Rx_t rx;

    // Same frame, different address: the codeword is valid and arrives in the
    // slot we watch, so only the address comparison keeps it out.
    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 1578624, 1578624 + 8, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "NOPE", false);

    CHECK(ev != POCSAG_RX_EVENT_PAGE, "decoded a page addressed to someone else");
    CHECK(rx.msg_len == 0, "kept text \"%s\" from another address", rx.msg);
    CHECK(rx.bad == 0, "%u bad codewords in a clean stream", rx.bad);
}

/* POCSAG_RX_MONITOR_RIC turns the address comparison off, for watching a channel
 * rather than waiting on one address. Checked both ways round: that a page to
 * somebody else is kept, and that it still reports whose it was, since without
 * the address a monitored page is a message from nobody. */
static void TestMonitorKeepsEveryPage(void)
{
    static const uint32_t others[] = { 1578624, 1069132, 8, 2097151, 42 };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        POCSAG_Rx_t rx;

        const POCSAG_RxEvent_t ev =
            Roundtrip(&rx, others[i], POCSAG_RX_MONITOR_RIC,
                      POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "ANYONE", false);

        CHECK(ev == POCSAG_RX_EVENT_PAGE, "monitor missed a page to %u", others[i]);
        CHECK(rx.page_ric == others[i], "monitor reported %u for a page to %u",
              rx.page_ric, others[i]);
        CHECK(strcmp(rx.msg, "ANYONE") == 0, "monitor msg \"%s\"", rx.msg);
    }

    // And the filtering still works when an address is set, so that monitoring
    // is something you opt into rather than the default.
    POCSAG_Rx_t rx;

    CHECK(Roundtrip(&rx, 1578624, 1069132, POCSAG_FUNC_D, POCSAG_MSG_ALPHA,
                    "NOT YOURS", false) != POCSAG_RX_EVENT_PAGE,
          "a set address stopped filtering");
}

static void TestInverted(void)
{
    POCSAG_Rx_t rx;
    const uint32_t ric = 683607;

    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, ric, ric, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "UPSIDE DOWN", true);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page from inverted data");
    CHECK(strcmp(rx.msg, "UPSIDE DOWN") == 0, "msg \"%s\"", rx.msg);
}

static void TestMultiBatch(void)
{
    POCSAG_Rx_t rx;
    char msg[POCSAG_RX_MAX_CHARS + 1];

    for (uint8_t i = 0; i < POCSAG_RX_MAX_CHARS; i++)
        msg[i] = (char)('A' + (i % 26));
    msg[POCSAG_RX_MAX_CHARS] = '\0';

    // 60 characters is 21 message codewords, so this crosses at least one batch
    // boundary and depends on the inter batch sync word being found.
    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 1578624, 1578624, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, msg, false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page across batches");
    CHECK(strcmp(rx.msg, msg) == 0, "msg \"%s\"", rx.msg);
    CHECK(!rx.truncated, "reported truncated at exactly the limit");
}

static void TestTruncation(void)
{
    POCSAG_Rx_t rx;
    char msg[MAX_CHARS + 1];

    for (uint16_t i = 0; i < MAX_CHARS; i++)
        msg[i] = (char)('a' + (i % 26));
    msg[MAX_CHARS] = '\0';

    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 7, 7, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, msg, false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page");
    CHECK(rx.truncated, "a message past the limit should say so");
    CHECK(rx.msg_len == POCSAG_RX_MAX_CHARS, "kept %u characters", rx.msg_len);
    CHECK(strncmp(rx.msg, msg, POCSAG_RX_MAX_CHARS) == 0, "msg \"%s\"", rx.msg);
}

static void TestSingleBitError(void)
{
    // A bit error in every codeword of the page in turn, one run each. BCH(31,21)
    // corrects one error per codeword, so all of them should still decode.
    const char *want = "CORRECT ME";

    for (uint32_t cw = 0; cw < 6; cw++) {
        for (uint32_t bit = 0; bit < 32; bit += 7) {
            POCSAG_Rx_t rx;
            const uint32_t len = Encode(1578624, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, want);

            // Skip the sync word the modem consumed, then index codewords in the
            // data the decoder actually sees.
            FlipBit(gBuf + LEAD_BYTES, cw * 32 + bit);

            POCSAG_RxInit(&rx, 1578624, false);
            POCSAG_RxResync(&rx, false);

            const POCSAG_RxEvent_t ev = FeedChunked(&rx, Data(), len);

            CHECK(ev == POCSAG_RX_EVENT_PAGE,
                  "codeword %u bit %u: no page", cw, bit);
            CHECK(strcmp(rx.msg, want) == 0,
                  "codeword %u bit %u: msg \"%s\"", cw, bit, rx.msg);
            CHECK(rx.corrected >= 1,
                  "codeword %u bit %u: no correction counted", cw, bit);
        }
    }
}

static void TestSyncRecovery(void)
{
    POCSAG_Rx_t rx;
    char msg[POCSAG_RX_MAX_CHARS + 1];

    for (uint8_t i = 0; i < POCSAG_RX_MAX_CHARS; i++)
        msg[i] = (char)('A' + (i % 26));
    msg[POCSAG_RX_MAX_CHARS] = '\0';

    const uint32_t len = Encode(8, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, msg);

    // The address is in frame 1, so the page runs well past the first batch.
    // Wreck the sync word between batch 1 and 2 beyond the match tolerance and
    // the decoder has to fall back to hunting bit by bit.
    uint8_t *data = gBuf + LEAD_BYTES;

    for (uint32_t b = 0; b < 8; b++)
        FlipBit(data, POCSAG_SLOTS_PER_BATCH * 32u + b * 3);

    POCSAG_RxInit(&rx, 8, false);
    POCSAG_RxResync(&rx, false);

    FeedChunked(&rx, data, len);

    // What matters is not that this page survives, it cannot, but that the
    // decoder gets back in step instead of decoding rubbish for ever after.
    CHECK(rx.locked || rx.hunt_bits > 0, "decoder neither locked nor hunting");

    // A clean page straight afterwards must decode, which is the real test that
    // hunting recovers.
    const uint32_t len2 = Encode(8, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "AFTER");
    POCSAG_RxResync(&rx, false);
    const POCSAG_RxEvent_t ev = FeedChunked(&rx, Data(), len2);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page after recovering framing");
    CHECK(strcmp(rx.msg, "AFTER") == 0, "msg \"%s\"", rx.msg);
}

static void TestNoise(void)
{
    // A cheap deterministic pseudo random stream. Nothing in it should ever be
    // accepted as a page: the sync word has to be found before any codeword is
    // looked at, and a valid address for our own RIC on top of that.
    POCSAG_Rx_t rx;
    uint8_t noise[2048];
    uint32_t x = 0x12345678u;

    for (uint32_t i = 0; i < sizeof(noise); i++) {
        x = x * 1103515245u + 12345u;
        noise[i] = (uint8_t)(x >> 16);
    }

    POCSAG_RxInit(&rx, 1578624, false);

    const POCSAG_RxEvent_t ev = FeedChunked(&rx, noise, sizeof(noise));

    CHECK(ev != POCSAG_RX_EVENT_PAGE, "decoded a page out of noise");
    CHECK(rx.msg_len == 0, "noise produced text \"%s\"", rx.msg);
    CHECK(rx.hunt_bits > 0, "decoder should still be hunting after noise");
}

/* The receive session decides which way round the RX FIFO hands over each word
 * by watching whether a packet produces any valid codewords at all. That test is
 * only sound if the wrong order really does produce none, so check it here
 * rather than leave the session resting on an assumption. */
static void TestSwappedPairsDecodeNothing(void)
{
    POCSAG_Rx_t rx;
    const uint32_t len = Encode(1578624, POCSAG_FUNC_D, POCSAG_MSG_ALPHA,
                                "EVERY BYTE PAIR THE WRONG WAY ROUND");
    uint8_t *data = gBuf + LEAD_BYTES;

    for (uint32_t i = 0; i + 1 < len; i += 2) {
        const uint8_t t = data[i];

        data[i]     = data[i + 1];
        data[i + 1] = t;
    }

    POCSAG_RxInit(&rx, 1578624, false);
    POCSAG_RxResync(&rx, false);

    const POCSAG_RxEvent_t ev = FeedChunked(&rx, data, len);

    CHECK(ev != POCSAG_RX_EVENT_PAGE, "transposed bytes decoded as a page");
    CHECK(rx.codewords == 0, "transposed bytes gave %u valid codewords, so the "
                             "session cannot use that to spot the wrong order",
          rx.codewords);
}

/* Builds a stream codeword by codeword, starting where the modem hands over, so
 * that framing cases the encoder will not produce on its own can be tested. */
static uint8_t         gRaw[512];
static POCSAG_BitBuf_t gRawBits;

static void RawInit(void)
{
    POCSAG_BufInit(&gRawBits, gRaw, sizeof(gRaw));
}

static void RawWord(uint32_t w)
{
    for (int8_t i = 31; i >= 0; i--) {
        const uint32_t bit = (w >> i) & 1u;

        if (bit)
            gRaw[gRawBits.length >> 3] |= (uint8_t)(1u << (7 - (gRawBits.length & 7)));

        gRawBits.length++;
    }
}

// One alphanumeric character followed by end of text, as a message codeword.
static uint32_t RawTextWord(char c)
{
    uint32_t acc = 0;

    for (uint8_t b = 0; b < 7; b++)
        acc = (acc << 1) | (uint32_t)(((uint8_t)c >> b) & 1u);

    for (uint8_t b = 0; b < 13; b++)
        acc = (acc << 1) | (uint32_t)((0x04u >> (b % 7)) & 1u);

    return POCSAG_MessageCodeword(acc);
}

/* A message that runs to the end of a batch, then another page to the same
 * address in the very first slot of the next one. The address codeword both ends
 * the first page and starts the second, and getting that wrong loses the first
 * message: it was being assembled in the same buffer the finished page is read
 * from, so starting the second page cleared it and reported an empty one. */
static void TestPageEndedByAnotherPage(void)
{
    POCSAG_Rx_t rx;
    const uint32_t ric = 1578624;    // frame 0, so slot 0 of every batch

    RawInit();

    // batch 1: our address, then message codewords all the way to slot 15
    RawWord(POCSAG_AddressCodeword(ric, POCSAG_FUNC_D));
    for (uint8_t slot = 1; slot < POCSAG_SLOTS_PER_BATCH; slot++)
        RawWord(RawTextWord((char)('A' + slot - 1)));

    // the inter batch sync word, then straight into another page to us
    RawWord(POCSAG_SYNC_CODEWORD);
    RawWord(POCSAG_AddressCodeword(ric, POCSAG_FUNC_D));
    RawWord(RawTextWord('Z'));
    for (uint8_t slot = 2; slot < POCSAG_SLOTS_PER_BATCH; slot++)
        RawWord(POCSAG_IDLE_CODEWORD);

    POCSAG_RxInit(&rx, ric, false);
    POCSAG_RxResync(&rx, false);

    // Feed a codeword at a time so each event can be inspected as it happens,
    // which is the only way to see the first page at all.
    char first[POCSAG_RX_MAX_CHARS + 1] = "";
    char second[POCSAG_RX_MAX_CHARS + 1] = "";
    uint8_t pages = 0;

    for (uint32_t off = 0; off < gRawBits.length / 8u; off += 4) {
        if (POCSAG_RxFeed(&rx, gRaw + off, 4) != POCSAG_RX_EVENT_PAGE)
            continue;

        if (pages == 0)
            strcpy(first, rx.msg);
        else if (pages == 1)
            strcpy(second, rx.msg);

        pages++;
    }

    CHECK(pages == 2, "%u pages, wanted 2", pages);
    CHECK(strcmp(first, "A") == 0, "first page \"%s\", wanted \"A\"", first);
    CHECK(strcmp(second, "Z") == 0, "second page \"%s\", wanted \"Z\"", second);
}

/* The receive session decides which way round the RX FIFO hands over each word,
 * and whether what it is hearing is POCSAG at all, from the idle codeword count.
 * Both of those rest on two claims that are checked here rather than assumed:
 * a real batch is mostly idles, and a stream that is not POCSAG has none.
 *
 * The codeword count cannot do this job. Both 0x00000000 and 0xFFFFFFFF are
 * valid BCH codewords, so a demodulator slicing receiver noise into long runs
 * fills the codeword count - and the address count - with words that were never
 * transmitted. An earlier version confirmed the byte order from the codeword
 * count and could therefore be fooled into latching the wrong one. */
static void TestIdleCountIsTheFingerprint(void)
{
    POCSAG_Rx_t rx;

    // A real page: the batch is padded with idles either side of the message.
    const POCSAG_RxEvent_t ev =
        Roundtrip(&rx, 1578624, 1578624, POCSAG_FUNC_D, POCSAG_MSG_ALPHA, "HI", false);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "no page");
    CHECK(rx.codewords == 16, "%u codewords in one batch, wanted 16", rx.codewords);
    CHECK(rx.idles >= 4, "only %u idles in a real batch", rx.idles);

    // Constant runs, which is what the demodulator produces once the carrier has
    // gone. These pass BCH and are counted as codewords and as addresses, so only
    // the idle count keeps them apart from a real transmission.
    static const struct { const char *name; uint8_t fill; } runs[] = {
        { "all zeroes", 0x00 }, { "all ones", 0xFF },
    };

    for (unsigned i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        uint8_t buf[256];

        memset(buf, runs[i].fill, sizeof(buf));

        POCSAG_RxInit(&rx, 1578624, false);
        POCSAG_RxResync(&rx, false);
        FeedChunked(&rx, buf, sizeof(buf));

        CHECK(rx.idles == 0, "%s produced %u idles, so the idle count cannot be "
                             "used to recognise a real transmission",
              runs[i].name, rx.idles);
        CHECK(POCSAG_RxFlush(&rx) != POCSAG_RX_EVENT_PAGE,
              "%s produced a page", runs[i].name);
    }
}

static void TestFlush(void)
{
    // A transmission cut off before its terminating idle codeword. The message
    // so far is still worth having, so the flush has to produce it.
    POCSAG_Rx_t rx;
    const uint32_t len = Encode(1578624, POCSAG_FUNC_D, POCSAG_MSG_ALPHA,
                                "CUT OFF PART WAY THROUGH THE MESSAGE");

    POCSAG_RxInit(&rx, 1578624, false);
    POCSAG_RxResync(&rx, false);

    /* Stop two codewords before the last text codeword, which lands inside the
     * message.
     *
     * Counting back a fixed amount from the end used to do this and no longer can:
     * the encoder appends POCSAG_TAIL_IDLES idles plus batch padding, which for a
     * message this length is longer than the message itself. So find the last
     * codeword that is neither idle nor sync - codewords are 4 byte aligned from the
     * start of data, which is where the modem hands over - and cut before it.
     *
     * The scan used to start at POCSAG_PREAMBLE_BITS / 8 while indexing data, which
     * has already had the preamble and the sync word removed. That put it 72 bytes too
     * deep, found no codeword at all, and left last_text at 0 - and the CHECK below
     * only reported it rather than returning, so the subtraction underflowed and fed
     * FeedChunked four gigabytes. This test never actually exercised POCSAG_RxFlush. */
    const uint8_t *data = Data();
    uint32_t       last_text = 0;

    for (uint32_t off = 0; off + 4u <= len; off += 4u) {
        const uint32_t cw = ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
                            ((uint32_t)data[off + 2] << 8) | data[off + 3];

        if (cw != POCSAG_IDLE_CODEWORD && cw != POCSAG_SYNC_CODEWORD)
            last_text = off;
    }

    // Returning rather than only reporting: last_text is about to be used as an
    // unsigned length, and 0 - 4 is four gigabytes.
    if (last_text < 8u) {
        CHECK(false, "no text codewords found past the address");
        return;
    }

    FeedChunked(&rx, data, last_text - 4u);

    const POCSAG_RxEvent_t ev = POCSAG_RxFlush(&rx);

    CHECK(ev == POCSAG_RX_EVENT_PAGE, "flush produced nothing");
    CHECK(rx.msg_len > 0, "flush produced an empty message");
    CHECK(strncmp(rx.msg, "CUT OFF PART WAY", 16) == 0, "msg \"%s\"", rx.msg);
    CHECK(POCSAG_RxFlush(&rx) == POCSAG_RX_EVENT_NONE, "flushed the same page twice");
}

int main(void)
{
    printf("POCSAG receive decoder\n");

    TestAlpha();
    TestPrintableRange();
    TestNumeric();
    TestTone();
    TestEveryFrame();
    TestWrongRic();
    TestMonitorKeepsEveryPage();
    TestInverted();
    TestMultiBatch();
    TestTruncation();
    TestSingleBitError();
    TestSyncRecovery();
    TestNoise();
    TestSwappedPairsDecodeNothing();
    TestIdleCountIsTheFingerprint();
    TestPageEndedByAnotherPage();
    TestFlush();

    if (failures == 0)
        printf("  all checks passed\n");
    else
        printf("  %d check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
