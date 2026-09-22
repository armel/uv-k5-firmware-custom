/* Host-side harness for the POCSAG encoder in app/pocsag.c.
 *
 * Renders a transmission as an FM-discriminator-style audio file so it can be
 * checked with multimon-ng, and self-tests the BCH encoder against the two
 * codewords whose values the standard fixes (sync and idle).
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/pocsag.h"

#define DEFAULT_RATE   22050   // multimon-ng only accepts this rate

struct options_t {
    uint32_t          ric;
    POCSAG_Function_t func;
    POCSAG_MsgType_t  type;
    uint32_t          baud;
    uint32_t          rate;
    uint32_t          repeat;
    bool              invert;
    bool              shape;
    bool              dump;
    const char       *out;
    const char       *msg;
};

static void WriteLE(FILE *f, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((value >> (8 * i)) & 0xFF, f);
}

static void WriteWavHeader(FILE *f, uint32_t rate, uint32_t samples)
{
    const uint32_t data_bytes = samples * 2;

    fwrite("RIFF", 1, 4, f);
    WriteLE(f, 36 + data_bytes, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    WriteLE(f, 16, 4);          // fmt chunk size
    WriteLE(f, 1, 2);           // PCM
    WriteLE(f, 1, 2);           // mono
    WriteLE(f, rate, 4);
    WriteLE(f, rate * 2, 4);    // byte rate
    WriteLE(f, 2, 2);           // block align
    WriteLE(f, 16, 2);          // bits per sample
    fwrite("data", 1, 4, f);
    WriteLE(f, data_bytes, 4);
}

static int SelfTest(void)
{
    const struct { const char *name; uint32_t cw; } vectors[] = {
        { "sync", POCSAG_SYNC_CODEWORD },
        { "idle", POCSAG_IDLE_CODEWORD },
    };
    int failures = 0;

    // Both are valid BCH(31,21) codewords with even parity, so re-encoding
    // their payload bits has to reproduce them exactly.
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const uint32_t got = POCSAG_BchEncode(vectors[i].cw);

        printf("  %-5s expect %08X  got %08X  %s\n",
               vectors[i].name, vectors[i].cw, got,
               (got == vectors[i].cw) ? "ok" : "FAIL");

        if (got != vectors[i].cw)
            failures++;
    }

    // A single bit flip anywhere must break the codeword.
    for (int bit = 0; bit < 32; bit++) {
        const uint32_t corrupt = POCSAG_SYNC_CODEWORD ^ (1u << bit);

        if (POCSAG_BchEncode(corrupt) == corrupt) {
            printf("  bit %2d flip of sync still validates  FAIL\n", bit);
            failures++;
        }
    }
    if (failures == 0)
        printf("  all 32 single bit flips of sync rejected  ok\n");

    // The frame a RIC lands in is its low 3 bits, and those bits are not
    // carried in the address codeword itself.
    for (uint32_t ric = 0; ric < 64; ric++) {
        const uint32_t a = POCSAG_AddressCodeword(ric, POCSAG_FUNC_D);
        const uint32_t b = POCSAG_AddressCodeword(ric ^ 7u, POCSAG_FUNC_D);

        if (a != b) {
            printf("  RIC %u and %u differ on air  FAIL\n", ric, ric ^ 7u);
            failures++;
            break;
        }
    }
    if (failures == 0)
        printf("  low 3 RIC bits absent from address codeword  ok\n");

    printf("%s\n", failures ? "SELF TEST FAILED" : "self test passed");
    return failures ? 1 : 0;
}

static void Usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options] [message]\n"
        "  -r N     RIC / capcode, 0 .. 2097151      (default 1234567)\n"
        "  -f N     function bits 0..3, 3 = alpha    (default 3)\n"
        "  -t T     tone | numeric | alpha           (default alpha)\n"
        "  -b N     baud 512 | 1200 | 2400           (default 1200)\n"
        "  -s N     sample rate                      (default %d)\n"
        "  -n N     repeat the transmission N times  (default 1)\n"
        "  -o FILE  output wav                       (default pocsag.wav)\n"
        "  -i       invert the on-air polarity\n"
        "  -p       low pass the waveform instead of emitting raw NRZ\n"
        "  -d       dump the codewords to stdout\n"
        "  -T       run the self test and exit\n",
        argv0, DEFAULT_RATE);
}

static void DumpCodewords(const POCSAG_BitBuf_t *buf)
{
    uint32_t i = POCSAG_PREAMBLE_BITS;
    uint32_t n = 0;

    printf("preamble %u bits\n", POCSAG_PREAMBLE_BITS);

    for (; i + 32 <= buf->length; i += 32, n++) {
        uint32_t cw = 0;
        for (int b = 0; b < 32; b++)
            cw = (cw << 1) | (POCSAG_BufGetBit(buf, i + b) ? 1u : 0u);

        const char *kind = (cw == POCSAG_SYNC_CODEWORD) ? "SYNC" :
                           (cw == POCSAG_IDLE_CODEWORD) ? "idle" :
                           (cw & 0x80000000u)           ? "msg"  : "ADDR";

        printf("  [%3u] %08X  %s\n", n, cw, kind);
    }
}

int main(int argc, char **argv)
{
    struct options_t opt = {
        .ric    = 1234567,
        .func   = POCSAG_FUNC_D,
        .type   = POCSAG_MSG_ALPHA,
        .baud   = 1200,
        .rate   = DEFAULT_RATE,
        .repeat = 1,
        .out    = "pocsag.wav",
        .msg    = "HELLO",
    };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] != '-') { opt.msg = a; continue; }
        if (!strcmp(a, "-T")) return SelfTest();
        if (!strcmp(a, "-i")) { opt.invert = true; continue; }
        if (!strcmp(a, "-p")) { opt.shape  = true; continue; }
        if (!strcmp(a, "-d")) { opt.dump   = true; continue; }
        if (!strcmp(a, "-h")) { Usage(argv[0]); return 0; }

        if (i + 1 >= argc) { Usage(argv[0]); return 2; }
        const char *v = argv[++i];

        if      (!strcmp(a, "-r")) opt.ric    = strtoul(v, NULL, 0);
        else if (!strcmp(a, "-f")) opt.func   = (POCSAG_Function_t)(strtoul(v, NULL, 0) & 3);
        else if (!strcmp(a, "-b")) opt.baud   = strtoul(v, NULL, 0);
        else if (!strcmp(a, "-s")) opt.rate   = strtoul(v, NULL, 0);
        else if (!strcmp(a, "-n")) opt.repeat = strtoul(v, NULL, 0);
        else if (!strcmp(a, "-o")) opt.out    = v;
        else if (!strcmp(a, "-t")) {
            if      (!strcmp(v, "tone"))    opt.type = POCSAG_MSG_TONE;
            else if (!strcmp(v, "numeric")) opt.type = POCSAG_MSG_NUMERIC;
            else if (!strcmp(v, "alpha"))   opt.type = POCSAG_MSG_ALPHA;
            else { Usage(argv[0]); return 2; }
        }
        else { Usage(argv[0]); return 2; }
    }

    const uint32_t capacity = POCSAG_EstimateBytes(opt.type, (uint32_t)strlen(opt.msg));
    uint8_t       *storage  = malloc(capacity);
    POCSAG_BitBuf_t buf;

    if (storage == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    POCSAG_BufInit(&buf, storage, capacity);

    if (!POCSAG_Encode(&buf, opt.ric, opt.func, opt.type, opt.msg)) {
        fprintf(stderr, "encode failed (overflow=%d, capacity=%u bytes)\n", buf.overflow, capacity);
        return 1;
    }

    if (opt.dump)
        DumpCodewords(&buf);

    FILE *f = fopen(opt.out, "wb");
    if (f == NULL) { perror(opt.out); return 1; }

    const uint32_t lead    = opt.rate / 20;   // 50ms of silence either side
    const int16_t  level   = 12000;
    uint32_t       written = 0;

    WriteWavHeader(f, opt.rate, 0);   // patched once the length is known

    for (uint32_t i = 0; i < lead; i++, written++)
        WriteLE(f, 0, 2);

    double   filtered = 0.0;
    uint64_t acc      = 0;

    for (uint32_t rep = 0; rep < opt.repeat; rep++) {
        for (uint32_t b = 0; b < buf.length; b++) {
            const bool bit = POCSAG_BufGetBit(&buf, b);

            // A binary '0' is the positive deviation, so it comes out of an FM
            // discriminator as a positive level.
            double target = bit ? -(double)level : (double)level;
            if (opt.invert)
                target = -target;

            acc += opt.rate;
            const uint32_t n = (uint32_t)(acc / opt.baud);
            acc %= opt.baud;

            for (uint32_t s = 0; s < n; s++, written++) {
                if (opt.shape)
                    filtered += (target - filtered) * 0.35;
                else
                    filtered = target;

                WriteLE(f, (uint16_t)(int16_t)filtered, 2);
            }
        }
    }

    for (uint32_t i = 0; i < lead; i++, written++)
        WriteLE(f, 0, 2);

    rewind(f);
    WriteWavHeader(f, opt.rate, written);
    fclose(f);
    free(storage);

    printf("%s: RIC %u func %d %u baud, %u bits, %u samples (%.2fs)\n",
           opt.out, opt.ric, (int)opt.func, opt.baud,
           buf.length * opt.repeat, written, (double)written / opt.rate);

    return 0;
}
