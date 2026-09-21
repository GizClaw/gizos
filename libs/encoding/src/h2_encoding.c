#include "h2_encoding.h"

#include <string.h>

#define INVALID_DIGIT 0xffu

static const char k_hex_lower[] = "0123456789abcdef";
static const char k_base32_std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char k_base32_hex[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
static const char k_base64_std[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char k_base64_url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char k_base85_ascii85[] =
    "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu";
static const char k_base85_rfc1924[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

const h2_encoding_t h2_encoding_hex = {H2_ENCODING_KIND_HEX, k_hex_lower, '\0', '\0'};
const h2_encoding_t h2_encoding_base32_std = {H2_ENCODING_KIND_BASE32, k_base32_std, '=', '\0'};
const h2_encoding_t h2_encoding_base32_hex = {H2_ENCODING_KIND_BASE32, k_base32_hex, '=', '\0'};
const h2_encoding_t h2_encoding_base64_std = {H2_ENCODING_KIND_BASE64, k_base64_std, '=', '\0'};
const h2_encoding_t h2_encoding_base64_url = {H2_ENCODING_KIND_BASE64, k_base64_url, '=', '\0'};
const h2_encoding_t h2_encoding_base64_raw_std = {H2_ENCODING_KIND_BASE64, k_base64_std, '\0', '\0'};
const h2_encoding_t h2_encoding_base64_raw_url = {H2_ENCODING_KIND_BASE64, k_base64_url, '\0', '\0'};
const h2_encoding_t h2_encoding_base85_ascii85 = {H2_ENCODING_KIND_BASE85, k_base85_ascii85, '\0', 'z'};
const h2_encoding_t h2_encoding_base85_rfc1924 = {H2_ENCODING_KIND_BASE85, k_base85_rfc1924, '\0', '\0'};

/* Radix-2^bits families: symbols per padded block and bits per symbol. */
typedef struct {
    unsigned bits;
    size_t block_chars;
    size_t block_bytes;
} bit_layout_t;

static bool is_printable(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x21u && u <= 0x7eu;
}

static size_t radix_of(h2_encoding_kind_t kind) {
    switch (kind) {
    case H2_ENCODING_KIND_HEX:
        return 16;
    case H2_ENCODING_KIND_BASE32:
        return 32;
    case H2_ENCODING_KIND_BASE64:
        return 64;
    case H2_ENCODING_KIND_BASE85:
        return 85;
    }
    return 0;
}

static bit_layout_t bit_layout_of(h2_encoding_kind_t kind) {
    switch (kind) {
    case H2_ENCODING_KIND_HEX:
        return (bit_layout_t){4, 2, 1};
    case H2_ENCODING_KIND_BASE32:
        return (bit_layout_t){5, 8, 5};
    default:
        return (bit_layout_t){6, 4, 3};
    }
}

static char other_case(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Validates the descriptor and, when map is non-NULL, fills the decode table
 * with digit values and INVALID_DIGIT for every other byte. */
static bool build_map(const h2_encoding_t *enc, uint8_t *map) {
    uint8_t local[256];
    if (map == NULL) {
        map = local;
    }
    if (enc == NULL || enc->alphabet == NULL) {
        return false;
    }
    size_t radix = radix_of(enc->kind);
    if (radix == 0) {
        return false;
    }
    bool pads = enc->kind == H2_ENCODING_KIND_BASE32 || enc->kind == H2_ENCODING_KIND_BASE64;
    if (enc->padding != '\0' && (!pads || !is_printable(enc->padding))) {
        return false;
    }
    if (enc->zero_group != '\0' &&
        (enc->kind != H2_ENCODING_KIND_BASE85 || !is_printable(enc->zero_group))) {
        return false;
    }
    memset(map, INVALID_DIGIT, 256);
    for (size_t i = 0; i < radix; ++i) {
        char c = enc->alphabet[i];
        unsigned char u = (unsigned char)c;
        if (!is_printable(c) || map[u] != INVALID_DIGIT || c == enc->padding ||
            c == enc->zero_group) {
            return false;
        }
        map[u] = (uint8_t)i;
    }
    if (enc->alphabet[radix] != '\0') {
        return false;
    }
    if (enc->kind == H2_ENCODING_KIND_HEX) {
        for (size_t i = 0; i < radix; ++i) {
            unsigned char folded = (unsigned char)other_case(enc->alphabet[i]);
            if (map[folded] == INVALID_DIGIT) {
                map[folded] = (uint8_t)i;
            }
        }
    }
    return true;
}

bool h2_encoding_is_valid(const h2_encoding_t *enc) { return build_map(enc, NULL); }

/* Base85 digits kept per final group of n bytes (1..3): n + 1. */
static bool base85_encoded_len(size_t src_len, size_t *out_len) {
    size_t groups = src_len / 4;
    size_t rem = src_len % 4;
    if (groups > (SIZE_MAX - 4) / 5) {
        return false;
    }
    *out_len = groups * 5 + (rem != 0 ? rem + 1 : 0);
    return true;
}

static bool bit_encoded_len(const h2_encoding_t *enc, size_t src_len, size_t *out_len) {
    bit_layout_t layout = bit_layout_of(enc->kind);
    size_t blocks = src_len / layout.block_bytes;
    size_t rem = src_len % layout.block_bytes;
    if (blocks > (SIZE_MAX - layout.block_chars) / layout.block_chars) {
        return false;
    }
    size_t len = blocks * layout.block_chars;
    if (rem != 0) {
        len += enc->padding != '\0' ? layout.block_chars
                                    : (rem * 8 + layout.bits - 1) / layout.bits;
    }
    *out_len = len;
    return true;
}

h2_encoding_err_t h2_encoding_max_encoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len) {
    if (out_len == NULL || !build_map(enc, NULL)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    bool ok = enc->kind == H2_ENCODING_KIND_BASE85 ? base85_encoded_len(src_len, out_len)
                                                   : bit_encoded_len(enc, src_len, out_len);
    return ok ? H2_ENCODING_OK : H2_ENCODING_ERR_INVALID_ARG;
}

h2_encoding_err_t h2_encoding_max_decoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len) {
    if (out_len == NULL || !build_map(enc, NULL)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    if (enc->kind == H2_ENCODING_KIND_BASE85) {
        if (enc->zero_group != '\0') {
            if (src_len > SIZE_MAX / 4) {
                return H2_ENCODING_ERR_INVALID_ARG;
            }
            *out_len = src_len * 4;
        } else {
            size_t rem = src_len % 5;
            *out_len = src_len / 5 * 4 + (rem > 1 ? rem - 1 : 0);
        }
        return H2_ENCODING_OK;
    }
    bit_layout_t layout = bit_layout_of(enc->kind);
    size_t rem = src_len % layout.block_chars;
    *out_len = src_len / layout.block_chars * layout.block_bytes + rem * layout.bits / 8;
    return H2_ENCODING_OK;
}

/* Encoders and decoders accept out == NULL to size or validate the result
 * first, so dst is untouched on every error. */
static size_t encode_bits(const h2_encoding_t *enc, const uint8_t *src, size_t src_len,
                          char *out) {
    bit_layout_t layout = bit_layout_of(enc->kind);
    unsigned mask = (1u << layout.bits) - 1u;
    uint32_t acc = 0;
    unsigned acc_bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < src_len; ++i) {
        acc = (acc << 8) | src[i];
        acc_bits += 8;
        while (acc_bits >= layout.bits) {
            acc_bits -= layout.bits;
            if (out != NULL) {
                out[n] = enc->alphabet[(acc >> acc_bits) & mask];
            }
            ++n;
        }
    }
    if (acc_bits > 0) {
        if (out != NULL) {
            out[n] = enc->alphabet[(acc << (layout.bits - acc_bits)) & mask];
        }
        ++n;
    }
    while (enc->padding != '\0' && n % layout.block_chars != 0) {
        if (out != NULL) {
            out[n] = enc->padding;
        }
        ++n;
    }
    return n;
}

static size_t encode_base85(const h2_encoding_t *enc, const uint8_t *src, size_t src_len,
                            char *out) {
    size_t n = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        size_t take = src_len - i < 4 ? src_len - i : 4;
        uint32_t value = 0;
        for (size_t j = 0; j < 4; ++j) {
            value = (value << 8) | (j < take ? src[i + j] : 0u);
        }
        if (take == 4 && value == 0 && enc->zero_group != '\0') {
            if (out != NULL) {
                out[n] = enc->zero_group;
            }
            ++n;
            continue;
        }
        char digits[5];
        for (int j = 4; j >= 0; --j) {
            digits[j] = enc->alphabet[value % 85u];
            value /= 85u;
        }
        if (out != NULL) {
            memcpy(out + n, digits, take + 1);
        }
        n += take + 1;
    }
    return n;
}

h2_encoding_err_t h2_encoding_encode(const h2_encoding_t *enc,
                                     const uint8_t *src, size_t src_len,
                                     char *dst, size_t dst_cap,
                                     size_t *out_len) {
    size_t bound = 0;
    if (out_len == NULL || (src == NULL && src_len != 0) || (dst == NULL && dst_cap != 0) ||
        h2_encoding_max_encoded_len(enc, src_len, &bound) != H2_ENCODING_OK) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    bool base85 = enc->kind == H2_ENCODING_KIND_BASE85;
    size_t need = bound;
    if (base85 && enc->zero_group != '\0') {
        need = encode_base85(enc, src, src_len, NULL);
    }
    *out_len = need;
    if (need > dst_cap) {
        return H2_ENCODING_ERR_NO_SPACE;
    }
    if (base85) {
        encode_base85(enc, src, src_len, dst);
    } else {
        encode_bits(enc, src, src_len, dst);
    }
    return H2_ENCODING_OK;
}

/* Tail symbol counts that carry at least one byte more than one symbol fewer. */
static bool tail_is_complete(size_t chars, unsigned bits) {
    return chars == 0 || (chars - 1) * bits / 8 < chars * bits / 8;
}

static h2_encoding_err_t decode_bits(const h2_encoding_t *enc, const uint8_t *map,
                                     const char *src, size_t src_len, uint8_t *out,
                                     size_t *out_len) {
    bit_layout_t layout = bit_layout_of(enc->kind);
    size_t data_len = src_len;
    if (enc->padding != '\0') {
        while (data_len > 0 && src[data_len - 1] == enc->padding) {
            --data_len;
        }
    }
    for (size_t i = 0; i < data_len; ++i) {
        if (map[(unsigned char)src[i]] == INVALID_DIGIT) {
            *out_len = i;
            return H2_ENCODING_ERR_CORRUPT;
        }
    }
    size_t tail = data_len % layout.block_chars;
    if (enc->padding != '\0') {
        size_t pad = src_len - data_len;
        size_t expected = tail == 0 ? 0 : layout.block_chars - tail;
        if (pad > expected) {
            *out_len = data_len + expected;
            return H2_ENCODING_ERR_CORRUPT;
        }
        if (pad < expected) {
            *out_len = src_len;
            return H2_ENCODING_ERR_CORRUPT;
        }
    }
    if (!tail_is_complete(tail, layout.bits)) {
        *out_len = data_len;
        return H2_ENCODING_ERR_CORRUPT;
    }
    unsigned spare = (unsigned)(tail * layout.bits % 8);
    if (spare != 0 && (map[(unsigned char)src[data_len - 1]] & ((1u << spare) - 1u)) != 0) {
        *out_len = data_len - 1;
        return H2_ENCODING_ERR_CORRUPT;
    }
    uint32_t acc = 0;
    unsigned acc_bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < data_len; ++i) {
        acc = (acc << layout.bits) | map[(unsigned char)src[i]];
        acc_bits += layout.bits;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            if (out != NULL) {
                out[n] = (uint8_t)(acc >> acc_bits);
            }
            ++n;
        }
    }
    *out_len = n;
    return H2_ENCODING_OK;
}

static const uint64_t k_pow85[] = {1u, 85u, 7225u, 614125u, 52200625u, UINT64_C(4437053125)};

static h2_encoding_err_t decode_base85(const h2_encoding_t *enc, const uint8_t *map,
                                       const char *src, size_t src_len, uint8_t *out,
                                       size_t *out_len) {
    size_t n = 0;
    size_t i = 0;
    while (i < src_len) {
        if (enc->zero_group != '\0' && src[i] == enc->zero_group) {
            if (out != NULL) {
                memset(out + n, 0, 4);
            }
            n += 4;
            ++i;
            continue;
        }
        size_t start = i;
        size_t count = 0;
        uint64_t value = 0;
        while (count < 5 && i < src_len) {
            uint8_t digit = map[(unsigned char)src[i]];
            if (digit == INVALID_DIGIT) {
                *out_len = i;
                return H2_ENCODING_ERR_CORRUPT;
            }
            value = value * 85u + digit;
            ++count;
            ++i;
        }
        if (count == 1) {
            *out_len = src_len;
            return H2_ENCODING_ERR_CORRUPT;
        }
        size_t bytes = count - 1;
        if (count == 5) {
            if (value > UINT32_MAX || (value == 0 && enc->zero_group != '\0')) {
                *out_len = start;
                return H2_ENCODING_ERR_CORRUPT;
            }
            bytes = 4;
        } else {
            /* The encoder drops the low 5 - count digits of the zero-padded
             * group, so the payload is the unique multiple of 256^missing in
             * [prefix * 85^missing, (prefix + 1) * 85^missing). */
            size_t missing = 5 - count;
            uint64_t unit = (uint64_t)1 << (8 * missing);
            uint64_t low = value * k_pow85[missing];
            uint64_t payload = (low + unit - 1) / unit;
            if (payload >= ((uint64_t)1 << (8 * bytes)) ||
                payload * unit >= low + k_pow85[missing]) {
                *out_len = start;
                return H2_ENCODING_ERR_CORRUPT;
            }
            value = payload << (8 * missing);
        }
        if (out != NULL) {
            for (size_t j = 0; j < bytes; ++j) {
                out[n + j] = (uint8_t)(value >> (24 - 8 * j));
            }
        }
        n += bytes;
    }
    *out_len = n;
    return H2_ENCODING_OK;
}

h2_encoding_err_t h2_encoding_decode(const h2_encoding_t *enc,
                                     const char *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap,
                                     size_t *out_len) {
    uint8_t map[256];
    if (out_len == NULL || (src == NULL && src_len != 0) || (dst == NULL && dst_cap != 0) ||
        !build_map(enc, map)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    bool base85 = enc->kind == H2_ENCODING_KIND_BASE85;
    size_t need = 0;
    h2_encoding_err_t err = base85 ? decode_base85(enc, map, src, src_len, NULL, &need)
                                   : decode_bits(enc, map, src, src_len, NULL, &need);
    *out_len = need;
    if (err != H2_ENCODING_OK) {
        return err;
    }
    if (need > dst_cap) {
        return H2_ENCODING_ERR_NO_SPACE;
    }
    return base85 ? decode_base85(enc, map, src, src_len, dst, out_len)
                  : decode_bits(enc, map, src, src_len, dst, out_len);
}
