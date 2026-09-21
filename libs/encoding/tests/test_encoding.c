#include "h2_encoding.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct {
    const h2_encoding_t *enc;
    const char *plain;
    size_t plain_len;
    const char *text;
} vector_t;

static const h2_encoding_t *const k_all[] = {
    &h2_encoding_hex,           &h2_encoding_base32_std,     &h2_encoding_base32_hex,
    &h2_encoding_base64_std,    &h2_encoding_base64_url,     &h2_encoding_base64_raw_std,
    &h2_encoding_base64_raw_url, &h2_encoding_base85_ascii85, &h2_encoding_base85_rfc1924,
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define S(lit) lit, sizeof(lit) - 1

/* RFC 4648 section 10 vectors, Python base64.a85encode/b85encode output, and
 * cases that exercise alphabet tails and the Ascii85 zero group. */
static const vector_t k_vectors[] = {
    {&h2_encoding_hex, S(""), ""},
    {&h2_encoding_hex, S("f"), "66"},
    {&h2_encoding_hex, S("foobar"), "666f6f626172"},
    {&h2_encoding_hex, S("\x00\xff\x10"), "00ff10"},
    {&h2_encoding_base32_std, S(""), ""},
    {&h2_encoding_base32_std, S("f"), "MY======"},
    {&h2_encoding_base32_std, S("fo"), "MZXQ===="},
    {&h2_encoding_base32_std, S("foo"), "MZXW6==="},
    {&h2_encoding_base32_std, S("foob"), "MZXW6YQ="},
    {&h2_encoding_base32_std, S("fooba"), "MZXW6YTB"},
    {&h2_encoding_base32_std, S("foobar"), "MZXW6YTBOI======"},
    {&h2_encoding_base32_std, S("\xff"), "74======"},
    {&h2_encoding_base32_hex, S("f"), "CO======"},
    {&h2_encoding_base32_hex, S("fo"), "CPNG===="},
    {&h2_encoding_base32_hex, S("foo"), "CPNMU==="},
    {&h2_encoding_base32_hex, S("foob"), "CPNMUOG="},
    {&h2_encoding_base32_hex, S("fooba"), "CPNMUOJ1"},
    {&h2_encoding_base32_hex, S("foobar"), "CPNMUOJ1E8======"},
    {&h2_encoding_base64_std, S(""), ""},
    {&h2_encoding_base64_std, S("f"), "Zg=="},
    {&h2_encoding_base64_std, S("fo"), "Zm8="},
    {&h2_encoding_base64_std, S("foo"), "Zm9v"},
    {&h2_encoding_base64_std, S("foob"), "Zm9vYg=="},
    {&h2_encoding_base64_std, S("fooba"), "Zm9vYmE="},
    {&h2_encoding_base64_std, S("foobar"), "Zm9vYmFy"},
    {&h2_encoding_base64_std, S("\xfb\xff"), "+/8="},
    {&h2_encoding_base64_url, S("\xfb\xff"), "-_8="},
    {&h2_encoding_base64_raw_std, S("f"), "Zg"},
    {&h2_encoding_base64_raw_std, S("fo"), "Zm8"},
    {&h2_encoding_base64_raw_std, S("foobar"), "Zm9vYmFy"},
    {&h2_encoding_base64_raw_url, S("\xfb\xff"), "-_8"},
    {&h2_encoding_base85_ascii85, S(""), ""},
    {&h2_encoding_base85_ascii85, S("f"), "Ac"},
    {&h2_encoding_base85_ascii85, S("fo"), "Ao@"},
    {&h2_encoding_base85_ascii85, S("foo"), "AoDS"},
    {&h2_encoding_base85_ascii85, S("foob"), "AoDTs"},
    {&h2_encoding_base85_ascii85, S("fooba"), "AoDTs@/"},
    {&h2_encoding_base85_ascii85, S("foobar"), "AoDTs@<)"},
    {&h2_encoding_base85_ascii85, S("\x00\x00\x00\x00\xff\xff\xff\xff\x00"), "zs8W-!!!"},
    {&h2_encoding_base85_ascii85, S("\xff"), "rr"},
    {&h2_encoding_base85_ascii85, S("\xff\xff"), "s8N"},
    {&h2_encoding_base85_ascii85, S("\xff\xff\xff"), "s8W*"},
    {&h2_encoding_base85_ascii85, S("\x00"), "!!"},
    {&h2_encoding_base85_rfc1924, S("f"), "W&"},
    {&h2_encoding_base85_rfc1924, S("fo"), "W^V"},
    {&h2_encoding_base85_rfc1924, S("foo"), "W^Zo"},
    {&h2_encoding_base85_rfc1924, S("foob"), "W^Zp|"},
    {&h2_encoding_base85_rfc1924, S("fooba"), "W^Zp|VE"},
    {&h2_encoding_base85_rfc1924, S("foobar"), "W^Zp|VR8"},
    {&h2_encoding_base85_rfc1924, S("\x00\x00\x00\x00\xff\xff\xff\xff\x00"), "00000|NsC000"},
};

static void check_vector(const vector_t *v) {
    const uint8_t *plain = (const uint8_t *)v->plain;
    size_t text_len = strlen(v->text);
    char text[64];
    uint8_t bytes[64];
    size_t n = 0;

    CHECK(h2_encoding_max_encoded_len(v->enc, v->plain_len, &n) == H2_ENCODING_OK);
    CHECK(n >= text_len);
    CHECK(h2_encoding_encode(v->enc, plain, v->plain_len, NULL, 0, &n) ==
          (text_len == 0 ? H2_ENCODING_OK : H2_ENCODING_ERR_NO_SPACE));
    CHECK(n == text_len);
    memset(text, '#', sizeof(text));
    CHECK(h2_encoding_encode(v->enc, plain, v->plain_len, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == text_len && memcmp(text, v->text, n) == 0 && text[n] == '#');

    CHECK(h2_encoding_max_decoded_len(v->enc, text_len, &n) == H2_ENCODING_OK);
    CHECK(n >= v->plain_len);
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(h2_encoding_decode(v->enc, v->text, text_len, bytes, sizeof(bytes), &n) ==
          H2_ENCODING_OK);
    CHECK(n == v->plain_len && memcmp(bytes, plain, n) == 0 && bytes[n] == 0xa5);
    if (v->plain_len > 0) {
        memset(bytes, 0xa5, sizeof(bytes));
        CHECK(h2_encoding_decode(v->enc, v->text, text_len, bytes, v->plain_len - 1, &n) ==
              H2_ENCODING_ERR_NO_SPACE);
        CHECK(n == v->plain_len);
    }
}

static void check_corrupt(const h2_encoding_t *enc, const char *text, size_t offset) {
    uint8_t bytes[64];
    size_t n = 12345;
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(h2_encoding_decode(enc, text, strlen(text), bytes, sizeof(bytes), &n) ==
          H2_ENCODING_ERR_CORRUPT);
    if (n != offset) {
        fprintf(stderr, "corrupt '%s': offset %zu, want %zu\n", text, n, offset);
        exit(1);
    }
    /* Corruption wins over lack of space. */
    n = 12345;
    CHECK(h2_encoding_decode(enc, text, strlen(text), NULL, 0, &n) == H2_ENCODING_ERR_CORRUPT);
    CHECK(n == offset);
}

static void test_corrupt_inputs(void) {
    /* Hex: odd length, invalid symbol, embedded whitespace and NUL. */
    check_corrupt(&h2_encoding_hex, "abc", 3);
    check_corrupt(&h2_encoding_hex, "0g", 1);
    check_corrupt(&h2_encoding_hex, "00 11", 2);
    CHECK(h2_encoding_decode(&h2_encoding_hex, "0\0", 2, NULL, 0, &(size_t){0}) ==
          H2_ENCODING_ERR_CORRUPT);

    /* Padded base64: missing or excess padding, padding mid-input,
     * impossible tail length, nonzero trailing bits, wrong alphabet. */
    check_corrupt(&h2_encoding_base64_std, "Zg", 2);
    check_corrupt(&h2_encoding_base64_std, "Zg=", 3);
    check_corrupt(&h2_encoding_base64_std, "Zm9v=", 4);
    check_corrupt(&h2_encoding_base64_std, "Zm9v====", 4);
    check_corrupt(&h2_encoding_base64_std, "====", 0);
    check_corrupt(&h2_encoding_base64_std, "Zg==Zg==", 2);
    check_corrupt(&h2_encoding_base64_std, "Z===", 1);
    check_corrupt(&h2_encoding_base64_std, "Zh==", 1);
    check_corrupt(&h2_encoding_base64_std, "Zm9=", 2);
    check_corrupt(&h2_encoding_base64_std, "-_8=", 0);
    check_corrupt(&h2_encoding_base64_std, "Zm9v\n", 4);
    check_corrupt(&h2_encoding_base64_url, "+/8=", 0);

    /* Raw base64 rejects padding and one-symbol tails. */
    check_corrupt(&h2_encoding_base64_raw_std, "Zg==", 2);
    check_corrupt(&h2_encoding_base64_raw_std, "Zm9vY", 5);
    check_corrupt(&h2_encoding_base64_raw_std, "Zh", 1);

    /* Base32: tails of 1, 3 and 6 symbols carry no complete byte. */
    check_corrupt(&h2_encoding_base32_std, "M=======", 1);
    check_corrupt(&h2_encoding_base32_std, "MZX=====", 3);
    check_corrupt(&h2_encoding_base32_std, "MZXW6Y==", 6);
    check_corrupt(&h2_encoding_base32_std, "MZ======", 1);
    check_corrupt(&h2_encoding_base32_std, "my======", 0);
    check_corrupt(&h2_encoding_base32_std, "MY=====", 7);

    /* Base85: dangling digit, group overflow, zero group rules,
     * non-canonical final group, foreign symbols. */
    check_corrupt(&h2_encoding_base85_ascii85, "AoDTs@", 6);
    check_corrupt(&h2_encoding_base85_ascii85, "s8W-\"", 0);
    check_corrupt(&h2_encoding_base85_ascii85, "uuuuu", 0);
    check_corrupt(&h2_encoding_base85_ascii85, "AoDTss8W-\"", 5);
    check_corrupt(&h2_encoding_base85_ascii85, "!!!!!", 0);
    check_corrupt(&h2_encoding_base85_ascii85, "!!z!!", 2);
    check_corrupt(&h2_encoding_base85_ascii85, "!\"", 0);
    check_corrupt(&h2_encoding_base85_ascii85, "Ac~", 2);
    check_corrupt(&h2_encoding_base85_ascii85, "Ac v", 2);
    check_corrupt(&h2_encoding_base85_rfc1924, "|NsC1", 0);
    check_corrupt(&h2_encoding_base85_rfc1924, "~~~~~", 0);
    check_corrupt(&h2_encoding_base85_rfc1924, "z", 1);
    check_corrupt(&h2_encoding_base85_rfc1924, "W&\"", 2);
}

static void test_round_trips(void) {
    uint8_t plain[70];
    char text[256];
    uint8_t back[70];
    for (size_t i = 0; i < sizeof(plain); ++i) {
        plain[i] = (uint8_t)(i * 37u + 11u);
    }
    plain[20] = plain[21] = plain[22] = plain[23] = 0;
    for (size_t e = 0; e < COUNT(k_all); ++e) {
        for (size_t len = 0; len <= sizeof(plain); ++len) {
            size_t bound = 0;
            size_t n = 0;
            size_t m = 0;
            CHECK(h2_encoding_max_encoded_len(k_all[e], len, &bound) == H2_ENCODING_OK);
            CHECK(h2_encoding_encode(k_all[e], plain, len, text, sizeof(text), &n) ==
                  H2_ENCODING_OK);
            CHECK(n <= bound);
            CHECK(k_all[e]->zero_group != '\0' || n == bound);
            CHECK(h2_encoding_max_decoded_len(k_all[e], n, &bound) == H2_ENCODING_OK);
            CHECK(bound >= len);
            CHECK(h2_encoding_decode(k_all[e], text, n, back, sizeof(back), &m) ==
                  H2_ENCODING_OK);
            CHECK(m == len && memcmp(back, plain, len) == 0);
            if (n > 0) {
                CHECK(h2_encoding_encode(k_all[e], plain, len, text, n - 1, &m) ==
                      H2_ENCODING_ERR_NO_SPACE);
                CHECK(m == n);
                CHECK(h2_encoding_encode(k_all[e], plain, len, text, sizeof(text), &m) ==
                      H2_ENCODING_OK);
            }
            if (len > 0) {
                CHECK(h2_encoding_decode(k_all[e], text, n, back, len - 1, &m) ==
                      H2_ENCODING_ERR_NO_SPACE);
                CHECK(m == len);
                CHECK(h2_encoding_decode(k_all[e], text, n, NULL, 0, &m) ==
                      H2_ENCODING_ERR_NO_SPACE);
                CHECK(m == len);
            }
        }
    }
}

static void test_chunked_encoding(void) {
    static const uint8_t plain[] = "chunked encoding keeps block boundaries";
    size_t len = sizeof(plain) - 1;
    /* Indexed by kind: any size for hex, then the block size of each family. */
    const size_t chunk_bytes[] = {0, 1, 5, 3, 4};
    for (size_t e = 0; e < COUNT(k_all); ++e) {
        size_t chunk = chunk_bytes[k_all[e]->kind];
        char whole[128];
        char pieces[128];
        size_t whole_len = 0;
        size_t pieces_len = 0;
        CHECK(h2_encoding_encode(k_all[e], plain, len, whole, sizeof(whole), &whole_len) ==
              H2_ENCODING_OK);
        for (size_t off = 0; off < len; off += chunk) {
            size_t take = len - off < chunk ? len - off : chunk;
            size_t n = 0;
            CHECK(h2_encoding_encode(k_all[e], plain + off, take, pieces + pieces_len,
                                     sizeof(pieces) - pieces_len, &n) == H2_ENCODING_OK);
            pieces_len += n;
        }
        CHECK(pieces_len == whole_len && memcmp(pieces, whole, whole_len) == 0);
    }
}

static void test_custom_alphabets(void) {
    /* bcrypt-style base64 alphabet without padding. */
    h2_encoding_t crypt;
    char text[16];
    uint8_t bytes[16];
    size_t n = 0;
    CHECK(h2_encoding_init(&crypt, H2_ENCODING_KIND_BASE64,
                           "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                           '\0', '\0') == H2_ENCODING_OK);
    CHECK(h2_encoding_encode(&crypt, (const uint8_t *)"\x00\xff", 2, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 3 && memcmp(text, ".N6", 3) == 0);
    CHECK(h2_encoding_decode(&crypt, ".N6", 3, bytes, sizeof(bytes), &n) == H2_ENCODING_OK);
    CHECK(n == 2 && bytes[0] == 0x00 && bytes[1] == 0xff);

    /* WithPadding equivalent: reuse a predefined alphabet with other padding. */
    h2_encoding_t star;
    CHECK(h2_encoding_init(&star, H2_ENCODING_KIND_BASE32, h2_encoding_base32_std.alphabet, '*',
                           '\0') == H2_ENCODING_OK);
    CHECK(h2_encoding_encode(&star, (const uint8_t *)"f", 1, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 8 && memcmp(text, "MY******", 8) == 0);
    CHECK(h2_encoding_decode(&star, "MY******", 8, bytes, sizeof(bytes), &n) == H2_ENCODING_OK);
    CHECK(n == 1 && bytes[0] == 'f');
    CHECK(h2_encoding_init(&star, H2_ENCODING_KIND_BASE32, h2_encoding_base32_std.alphabet, '\0',
                           '\0') == H2_ENCODING_OK);
    CHECK(h2_encoding_encode(&star, (const uint8_t *)"f", 1, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 2 && memcmp(text, "MY", 2) == 0);

    /* Uppercase hex output; decoding folds case in both directions. */
    h2_encoding_t upper;
    CHECK(h2_encoding_init(&upper, H2_ENCODING_KIND_HEX, "0123456789ABCDEF", '\0', '\0') ==
          H2_ENCODING_OK);
    CHECK(h2_encoding_encode(&upper, (const uint8_t *)"\xab", 1, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 2 && memcmp(text, "AB", 2) == 0);
    CHECK(h2_encoding_decode(&upper, "aB", 2, bytes, sizeof(bytes), &n) == H2_ENCODING_OK);
    CHECK(n == 1 && bytes[0] == 0xab);
    CHECK(h2_encoding_decode(&h2_encoding_hex, "AbCdEf", 6, bytes, sizeof(bytes), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 3 && bytes[0] == 0xab && bytes[1] == 0xcd && bytes[2] == 0xef);

    /* A base85 zero group can be added to any base85 alphabet. */
    h2_encoding_t zeros;
    CHECK(h2_encoding_init(&zeros, H2_ENCODING_KIND_BASE85, h2_encoding_base85_rfc1924.alphabet,
                           '\0', '\'') == H2_ENCODING_OK);
    CHECK(h2_encoding_encode(&zeros, (const uint8_t *)"\0\0\0\0\0", 5, text, sizeof(text), &n) ==
          H2_ENCODING_OK);
    CHECK(n == 3 && memcmp(text, "'00", 3) == 0);
    CHECK(h2_encoding_decode(&zeros, "'00", 3, bytes, sizeof(bytes), &n) == H2_ENCODING_OK);
    CHECK(n == 5 && memcmp(bytes, "\0\0\0\0\0", 5) == 0);
}

/* Every predefined table must equal what h2_encoding_init builds. */
static void test_predefined_match_init(void) {
    for (size_t e = 0; e < COUNT(k_all); ++e) {
        const h2_encoding_t *want = k_all[e];
        h2_encoding_t got;
        CHECK(h2_encoding_init(&got, want->kind, want->alphabet, want->padding,
                               want->zero_group) == H2_ENCODING_OK);
        CHECK(got.kind == want->kind && got.padding == want->padding &&
              got.zero_group == want->zero_group);
        CHECK(memcmp(got.alphabet, want->alphabet, sizeof(got.alphabet)) == 0);
        CHECK(memcmp(got.decode_map, want->decode_map, sizeof(got.decode_map)) == 0);
    }
}

static void test_invalid_descriptors(void) {
    static const char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *a85 = h2_encoding_base85_ascii85.alphabet;
    const struct {
        int kind;
        const char *alphabet;
        char padding;
        char zero_group;
    } invalid[] = {
        {0, k_b64, '\0', '\0'},
        {5, k_b64, '\0', '\0'},
        {H2_ENCODING_KIND_BASE64, NULL, '\0', '\0'},
        {H2_ENCODING_KIND_BASE64, "ABC", '\0', '\0'},
        {H2_ENCODING_KIND_BASE32, k_b64, '\0', '\0'},
        {H2_ENCODING_KIND_BASE64,
         "AACDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", '\0', '\0'},
        {H2_ENCODING_KIND_BASE64,
         " BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", '\0', '\0'},
        {H2_ENCODING_KIND_BASE64,
         "\x80" "BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", '\0', '\0'},
        {H2_ENCODING_KIND_BASE64, k_b64, '+', '\0'},
        {H2_ENCODING_KIND_BASE64, k_b64, ' ', '\0'},
        {H2_ENCODING_KIND_BASE64, k_b64, '\n', '\0'},
        {H2_ENCODING_KIND_BASE64, k_b64, '\0', 'z'},
        {H2_ENCODING_KIND_HEX, "0123456789abcdef", '=', '\0'},
        {H2_ENCODING_KIND_BASE85, a85, '=', '\0'},
        {H2_ENCODING_KIND_BASE85, a85, '\0', 'u'},
        {H2_ENCODING_KIND_BASE85, a85, '\0', '\t'},
    };
    for (size_t i = 0; i < COUNT(invalid); ++i) {
        h2_encoding_t enc;
        memset(&enc, 0x5a, sizeof(enc));
        CHECK(h2_encoding_init(&enc, (h2_encoding_kind_t)invalid[i].kind, invalid[i].alphabet,
                               invalid[i].padding, invalid[i].zero_group) ==
              H2_ENCODING_ERR_INVALID_ARG);
        CHECK(enc.decode_map[0] == 0x5a && enc.alphabet[0] == 0x5a);
    }
    CHECK(h2_encoding_init(NULL, H2_ENCODING_KIND_BASE64, k_b64, '\0', '\0') ==
          H2_ENCODING_ERR_INVALID_ARG);

    /* Calls reject NULL and unknown-kind descriptors without touching outputs. */
    h2_encoding_t unknown = h2_encoding_base64_std;
    unknown.kind = (h2_encoding_kind_t)9;
    const h2_encoding_t *unusable[] = {NULL, &unknown};
    uint8_t byte = 0;
    char ch = 0;
    for (size_t i = 0; i < COUNT(unusable); ++i) {
        size_t n = 777;
        CHECK(h2_encoding_max_encoded_len(unusable[i], 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
        CHECK(h2_encoding_max_decoded_len(unusable[i], 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
        CHECK(h2_encoding_encode(unusable[i], &byte, 1, &ch, 1, &n) ==
              H2_ENCODING_ERR_INVALID_ARG);
        CHECK(h2_encoding_decode(unusable[i], "AA", 2, &byte, 1, &n) ==
              H2_ENCODING_ERR_INVALID_ARG);
        CHECK(n == 777 && byte == 0 && ch == 0);
    }
}

/* A hand-edited descriptor gives unspecified output but stays in bounds;
 * run under ASan to check. */
static void test_edited_descriptor_stays_in_bounds(void) {
    uint8_t plain[64];
    char text[160];
    uint8_t back[64];
    for (size_t i = 0; i < sizeof(plain); ++i) {
        plain[i] = (uint8_t)(i * 73u);
    }
    for (int kind = H2_ENCODING_KIND_HEX; kind <= H2_ENCODING_KIND_BASE85; ++kind) {
        h2_encoding_t enc;
        memset(&enc, 0, sizeof(enc));
        enc.kind = (h2_encoding_kind_t)kind;
        enc.padding = '=';
        enc.zero_group = 'z';
        for (size_t i = 0; i < sizeof(enc.decode_map); ++i) {
            enc.decode_map[i] = (uint8_t)(i * 151u);
        }
        size_t n = 0;
        size_t m = 0;
        (void)h2_encoding_encode(&enc, plain, sizeof(plain), text, sizeof(text), &n);
        for (size_t i = 0; i < sizeof(text); ++i) {
            text[i] = (char)(i * 29u);
        }
        (void)h2_encoding_decode(&enc, text, sizeof(text), back, sizeof(back), &m);
        (void)h2_encoding_decode(&enc, text, 7, back, 3, &m);
    }
}

static void test_invalid_arguments(void) {
    const h2_encoding_t *enc = &h2_encoding_base64_std;
    uint8_t byte = 0;
    char ch = 0;
    size_t n = 777;
    CHECK(h2_encoding_max_encoded_len(enc, 1, NULL) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_max_decoded_len(enc, 1, NULL) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_encode(enc, &byte, 1, &ch, 1, NULL) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_encode(enc, NULL, 1, &ch, 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_encode(enc, &byte, 1, NULL, 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_decode(enc, "Zg==", 4, &byte, 1, NULL) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_decode(enc, NULL, 1, &byte, 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_decode(enc, "Zg==", 4, NULL, 1, &n) == H2_ENCODING_ERR_INVALID_ARG);
    CHECK(n == 777);
    CHECK(h2_encoding_encode(enc, NULL, 0, NULL, 0, &n) == H2_ENCODING_OK && n == 0);
    CHECK(h2_encoding_decode(enc, NULL, 0, NULL, 0, &n) == H2_ENCODING_OK && n == 0);

    /* Sizes that do not fit in size_t are rejected instead of wrapping. */
    for (size_t e = 0; e < COUNT(k_all); ++e) {
        n = 777;
        CHECK(h2_encoding_max_encoded_len(k_all[e], SIZE_MAX, &n) ==
              H2_ENCODING_ERR_INVALID_ARG);
        CHECK(n == 777);
        CHECK(h2_encoding_encode(k_all[e], &byte, SIZE_MAX, &ch, 1, &n) ==
              H2_ENCODING_ERR_INVALID_ARG);
    }
    CHECK(h2_encoding_max_decoded_len(&h2_encoding_base85_ascii85, SIZE_MAX, &n) ==
          H2_ENCODING_ERR_INVALID_ARG);
    CHECK(h2_encoding_max_decoded_len(&h2_encoding_base64_std, SIZE_MAX, &n) ==
          H2_ENCODING_OK);
    CHECK(n <= SIZE_MAX / 4 * 3 + 2);
}

int main(void) {
    for (size_t i = 0; i < COUNT(k_vectors); ++i) {
        check_vector(&k_vectors[i]);
    }
    test_corrupt_inputs();
    test_round_trips();
    test_chunked_encoding();
    test_custom_alphabets();
    test_predefined_match_init();
    test_invalid_descriptors();
    test_edited_descriptor_stays_in_bounds();
    test_invalid_arguments();
    return 0;
}
