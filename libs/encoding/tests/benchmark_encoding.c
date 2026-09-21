#include "h2_encoding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP 4u
#define SAMPLES 64u
#define SAMPLE_BYTES (512u * 1024u)
#define MAX_PLAIN (64u * 1024u)

static uint8_t plain[MAX_PLAIN];
static uint8_t decoded[MAX_PLAIN];
static char text[MAX_PLAIN * 2u];
static volatile uint32_t sink;

typedef enum { OP_ENCODE, OP_DECODE, OP_DECODE_GROUPS, OP_GROUP_INLINE, OP_BASELINE } op_t;

static int compare(const void *a_value, const void *b_value) {
    double a = *(const double *)a_value, b = *(const double *)b_value;
    return (a > b) - (a < b);
}

static void check(h2_encoding_err_t err) {
    if (err != H2_ENCODING_OK) {
        fprintf(stderr, "benchmark call failed: %d\n", (int)err);
        exit(1);
    }
}

/* Display-style reference: bare table lookup over whole 5-digit groups with
 * no validation, the lower bound a streaming base85 reader can reach. */
static uint32_t baseline_base85(const uint8_t *map, const char *src, size_t len, uint8_t *out) {
    uint32_t any = 0;
    for (size_t i = 0; i + 5 <= len; i += 5, out += 4) {
        const unsigned char *g = (const unsigned char *)src + i;
        uint32_t v = (((map[g[0]] * 85u + map[g[1]]) * 85u + map[g[2]]) * 85u + map[g[3]]) * 85u +
                     map[g[4]];
        out[0] = (uint8_t)(v >> 24);
        out[1] = (uint8_t)(v >> 16);
        out[2] = (uint8_t)(v >> 8);
        out[3] = (uint8_t)v;
        any |= v;
    }
    return any;
}

static void run_once(op_t op, const h2_encoding_t *enc, size_t size, size_t text_len) {
    size_t n = 0;
    switch (op) {
    case OP_ENCODE:
        check(h2_encoding_encode(enc, plain, size, text, sizeof(text), &n));
        break;
    case OP_DECODE:
        check(h2_encoding_decode(enc, text, text_len, decoded, sizeof(decoded), &n));
        break;
    case OP_DECODE_GROUPS: {
        /* One call per block, as a streaming consumer would issue them. */
        size_t chars = enc->kind == H2_ENCODING_KIND_BASE85 ? 5 : 4;
        size_t bytes = enc->kind == H2_ENCODING_KIND_BASE85 ? 4 : 3;
        for (size_t i = 0, o = 0; i < text_len; i += chars, o += bytes) {
            check(h2_encoding_decode(enc, text + i, chars, decoded + o, bytes, &n));
        }
        break;
    }
    case OP_GROUP_INLINE:
        /* The inline single-group decoder, as streaming readers use it. */
        for (size_t i = 0, o = 0; i + 5 <= text_len; i += 5, o += 4) {
            uint32_t v;
            if (!h2_encoding_decode_base85_group(enc, text + i, &v)) {
                abort();
            }
            decoded[o] = (uint8_t)(v >> 24);
            decoded[o + 1] = (uint8_t)(v >> 16);
            decoded[o + 2] = (uint8_t)(v >> 8);
            decoded[o + 3] = (uint8_t)v;
        }
        break;
    case OP_BASELINE:
        sink ^= baseline_base85(enc->decode_map, text, text_len, decoded);
        break;
    }
    sink ^= (uint32_t)n ^ decoded[0] ^ (uint8_t)text[0];
}

static void measure(const char *name, op_t op, const h2_encoding_t *enc, size_t size) {
    static const char *const op_names[] = {"encode", "decode", "decode_per_group", "group_inline",
                                           "baseline"};
    size_t text_len = 0;
    check(h2_encoding_encode(enc, plain, size, text, sizeof(text), &text_len));
    unsigned repeats = (unsigned)(SAMPLE_BYTES / size);
    double samples[SAMPLES];
    for (unsigned sample = 0; sample < WARMUP + SAMPLES; ++sample) {
        clock_t start = clock();
        for (unsigned r = 0; r < repeats; ++r) {
            run_once(op, enc, size, text_len);
        }
        clock_t end = clock();
        if (start == (clock_t)-1 || end < start) {
            abort();
        }
        if (sample >= WARMUP) {
            samples[sample - WARMUP] = (double)(end - start) / CLOCKS_PER_SEC;
        }
    }
    qsort(samples, SAMPLES, sizeof(samples[0]), compare);
    double bytes = (double)size * repeats;
    double p50 = samples[SAMPLES / 2], p95 = samples[(SAMPLES * 95u) / 100u];
    printf("%-15s %-16s size=%-6zu p50_ns_per_byte=%6.3f p95_ns_per_byte=%6.3f "
           "p50_MiB_per_s=%8.1f\n",
           name, op_names[op], size, p50 * 1e9 / bytes, p95 * 1e9 / bytes,
           bytes / p50 / (1024.0 * 1024.0));
}

int main(void) {
    puts("encoding host CPU-time benchmark; warmup=4 samples=64, 512 KiB of plain "
         "bytes per sample; ns/byte and MiB/s are per plain (decoded) byte");
    printf("descriptor_bytes=%zu heap_allocations=0 per_call_setup=none\n",
           sizeof(h2_encoding_t));
    uint32_t seed = 0x2545f491u;
    for (size_t i = 0; i < sizeof(plain); ++i) {
        seed = seed * 1664525u + 1013904223u;
        plain[i] = (uint8_t)(seed >> 24);
    }
    const struct {
        const char *name;
        const h2_encoding_t *enc;
    } cases[] = {
        {"hex", &h2_encoding_hex},
        {"base32_std", &h2_encoding_base32_std},
        {"base64_std", &h2_encoding_base64_std},
        {"base85_ascii85", &h2_encoding_base85_ascii85},
        {"base85_rfc1924", &h2_encoding_base85_rfc1924},
    };
    const size_t sizes[] = {64, 4096, MAX_PLAIN};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            measure(cases[c].name, OP_ENCODE, cases[c].enc, sizes[s]);
            measure(cases[c].name, OP_DECODE, cases[c].enc, sizes[s]);
        }
    }
    measure("base64_std", OP_DECODE_GROUPS, &h2_encoding_base64_std, 4095);
    measure("base85_rfc1924", OP_DECODE_GROUPS, &h2_encoding_base85_rfc1924, MAX_PLAIN);
    measure("base85_rfc1924", OP_GROUP_INLINE, &h2_encoding_base85_rfc1924, MAX_PLAIN);
    measure("base85_rfc1924", OP_BASELINE, &h2_encoding_base85_rfc1924, MAX_PLAIN);
    return 0;
}
