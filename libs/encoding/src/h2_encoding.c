#include "h2_encoding.h"

#include <string.h>

#define INVALID_DIGIT 0xffu

/* UINT32_MAX == 85 * BASE85_MAX_HEAD, so a group's first four digits must not
 * exceed this value, and may equal it only when the fifth digit is 0. */
#define BASE85_MAX_HEAD 50529027u

/* Per-kind layout, indexed by h2_encoding_kind_t. Hex, base32 and base64 map
 * block_bytes input bytes to block_chars symbols of bits bits each. */
static const uint8_t k_radix[] = {0, 16, 32, 64, 85};
static const uint8_t k_bits[] = {0, 4, 5, 6, 0};
static const uint8_t k_block_chars[] = {0, 2, 8, 4, 5};
static const uint8_t k_block_bytes[] = {0, 1, 5, 3, 4};

static bool kind_is_valid(int kind) {
    return kind >= H2_ENCODING_KIND_HEX && kind <= H2_ENCODING_KIND_BASE85;
}

static bool descriptor_is_usable(const h2_encoding_t *enc) {
    return enc != NULL && kind_is_valid((int)enc->kind);
}

static bool is_printable(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x21u && u <= 0x7eu;
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

h2_encoding_err_t h2_encoding_init(h2_encoding_t *enc, h2_encoding_kind_t kind,
                                   const char *alphabet, char padding, char zero_group) {
    if (enc == NULL || alphabet == NULL || !kind_is_valid((int)kind)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    size_t radix = k_radix[kind];
    bool pads = kind == H2_ENCODING_KIND_BASE32 || kind == H2_ENCODING_KIND_BASE64;
    if (padding != '\0' && (!pads || !is_printable(padding))) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    if (zero_group != '\0' && (kind != H2_ENCODING_KIND_BASE85 || !is_printable(zero_group))) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    h2_encoding_t prepared;
    memset(&prepared, 0, sizeof(prepared));
    memset(prepared.decode_map, INVALID_DIGIT, sizeof(prepared.decode_map));
    for (size_t i = 0; i < radix; ++i) {
        char c = alphabet[i];
        unsigned char u = (unsigned char)c;
        if (!is_printable(c) || prepared.decode_map[u] != INVALID_DIGIT || c == padding ||
            c == zero_group) {
            return H2_ENCODING_ERR_INVALID_ARG;
        }
        prepared.decode_map[u] = (uint8_t)i;
        prepared.alphabet[i] = c;
    }
    if (alphabet[radix] != '\0') {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    if (kind == H2_ENCODING_KIND_HEX) {
        for (size_t i = 0; i < radix; ++i) {
            unsigned char folded = (unsigned char)other_case(alphabet[i]);
            if (prepared.decode_map[folded] == INVALID_DIGIT) {
                prepared.decode_map[folded] = (uint8_t)i;
            }
        }
    }
    prepared.kind = kind;
    prepared.padding = padding;
    prepared.zero_group = zero_group;
    *enc = prepared;
    return H2_ENCODING_OK;
}

static bool bits_encoded_len(const h2_encoding_t *enc, size_t src_len, size_t *out_len) {
    size_t chars = k_block_chars[enc->kind];
    size_t bytes = k_block_bytes[enc->kind];
    size_t blocks = src_len / bytes;
    size_t rem = src_len % bytes;
    if (blocks > (SIZE_MAX - chars) / chars) {
        return false;
    }
    size_t len = blocks * chars;
    if (rem != 0) {
        len += enc->padding != '\0' ? chars : (rem * 8 + k_bits[enc->kind] - 1) / k_bits[enc->kind];
    }
    *out_len = len;
    return true;
}

static bool base85_encoded_len(size_t src_len, size_t *out_len) {
    size_t groups = src_len / 4;
    size_t rem = src_len % 4;
    if (groups > (SIZE_MAX - 4) / 5) {
        return false;
    }
    *out_len = groups * 5 + (rem != 0 ? rem + 1 : 0);
    return true;
}

h2_encoding_err_t h2_encoding_max_encoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len) {
    if (out_len == NULL || !descriptor_is_usable(enc)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    bool ok = enc->kind == H2_ENCODING_KIND_BASE85 ? base85_encoded_len(src_len, out_len)
                                                   : bits_encoded_len(enc, src_len, out_len);
    return ok ? H2_ENCODING_OK : H2_ENCODING_ERR_INVALID_ARG;
}

h2_encoding_err_t h2_encoding_max_decoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len) {
    if (out_len == NULL || !descriptor_is_usable(enc)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    if (enc->kind == H2_ENCODING_KIND_BASE85 && enc->zero_group != '\0') {
        if (src_len > SIZE_MAX / 4) {
            return H2_ENCODING_ERR_INVALID_ARG;
        }
        *out_len = src_len * 4;
        return H2_ENCODING_OK;
    }
    size_t chars = k_block_chars[enc->kind];
    size_t rem = src_len % chars;
    size_t tail = enc->kind == H2_ENCODING_KIND_BASE85 ? (rem > 1 ? rem - 1 : 0)
                                                       : rem * k_bits[enc->kind] / 8;
    *out_len = src_len / chars * k_block_bytes[enc->kind] + tail;
    return H2_ENCODING_OK;
}

/* ---- Encoding ---------------------------------------------------------- */

static void encode_hex(const char *a, const uint8_t *src, size_t len, char *out) {
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = a[src[i] >> 4];
        out[2 * i + 1] = a[src[i] & 15u];
    }
}

static char *encode_base64_blocks(const char *a, const uint8_t *src, size_t blocks, char *out) {
    for (size_t i = 0; i < blocks; ++i, src += 3, out += 4) {
        uint32_t v = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];
        out[0] = a[v >> 18];
        out[1] = a[(v >> 12) & 63u];
        out[2] = a[(v >> 6) & 63u];
        out[3] = a[v & 63u];
    }
    return out;
}

static char *encode_base32_blocks(const char *a, const uint8_t *src, size_t blocks, char *out) {
    for (size_t i = 0; i < blocks; ++i, src += 5, out += 8) {
        /* Two 20-bit halves keep the arithmetic in 32-bit registers. */
        uint32_t hi = (uint32_t)src[0] << 12 | (uint32_t)src[1] << 4 | (uint32_t)src[2] >> 4;
        uint32_t lo = ((uint32_t)src[2] & 15u) << 16 | (uint32_t)src[3] << 8 | src[4];
        out[0] = a[hi >> 15];
        out[1] = a[(hi >> 10) & 31u];
        out[2] = a[(hi >> 5) & 31u];
        out[3] = a[hi & 31u];
        out[4] = a[lo >> 15];
        out[5] = a[(lo >> 10) & 31u];
        out[6] = a[(lo >> 5) & 31u];
        out[7] = a[lo & 31u];
    }
    return out;
}

/* Final partial block: rem bytes left-aligned in the block, the symbols that
 * cover them, then padding up to a whole block when configured. */
static void encode_bits_tail(const h2_encoding_t *enc, const uint8_t *src, size_t rem, char *out) {
    unsigned bits = k_bits[enc->kind];
    size_t block_bytes = k_block_bytes[enc->kind];
    size_t chars = (rem * 8 + bits - 1) / bits;
    uint64_t v = 0;
    for (size_t i = 0; i < block_bytes; ++i) {
        v = v << 8 | (i < rem ? src[i] : 0u);
    }
    unsigned shift = (unsigned)(block_bytes * 8);
    for (size_t i = 0; i < chars; ++i) {
        shift -= bits;
        out[i] = enc->alphabet[(v >> shift) & ((1u << bits) - 1u)];
    }
    if (enc->padding != '\0') {
        memset(out + chars, enc->padding, k_block_chars[enc->kind] - chars);
    }
}

static void encode_bits(const h2_encoding_t *enc, const uint8_t *src, size_t len, char *out) {
    const char *a = enc->alphabet;
    if (enc->kind == H2_ENCODING_KIND_HEX) {
        encode_hex(a, src, len, out);
        return;
    }
    size_t block_bytes = k_block_bytes[enc->kind];
    size_t blocks = len / block_bytes;
    out = enc->kind == H2_ENCODING_KIND_BASE64 ? encode_base64_blocks(a, src, blocks, out)
                                               : encode_base32_blocks(a, src, blocks, out);
    size_t rem = len % block_bytes;
    if (rem != 0) {
        encode_bits_tail(enc, src + blocks * block_bytes, rem, out);
    }
}

static void base85_digits(const char *a, uint32_t v, char digits[5]) {
    for (int i = 4; i >= 0; --i) {
        digits[i] = a[v % 85u];
        v /= 85u;
    }
}

/* Writes while dst has room and keeps counting afterwards, so a short buffer
 * still learns the exact size in the same single pass. */
static bool encode_base85(const h2_encoding_t *enc, const uint8_t *src, size_t len, char *dst,
                          size_t cap, size_t *out_len) {
    const char *a = enc->alphabet;
    char z = enc->zero_group;
    bool writing = true;
    size_t n = 0;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v = (uint32_t)src[i] << 24 | (uint32_t)src[i + 1] << 16 |
                     (uint32_t)src[i + 2] << 8 | src[i + 3];
        size_t k = (v == 0 && z != '\0') ? 1 : 5;
        if (writing && cap - n >= k) {
            if (k == 1) {
                dst[n] = z;
            } else {
                base85_digits(a, v, dst + n);
            }
        } else {
            writing = false;
        }
        n += k;
    }
    size_t rem = len - i;
    if (rem != 0) {
        uint32_t v = 0;
        for (size_t j = 0; j < 4; ++j) {
            v = v << 8 | (j < rem ? src[i + j] : 0u);
        }
        char digits[5];
        base85_digits(a, v, digits);
        if (writing && cap - n >= rem + 1) {
            memcpy(dst + n, digits, rem + 1);
        } else {
            writing = false;
        }
        n += rem + 1;
    }
    *out_len = n;
    return writing;
}

h2_encoding_err_t h2_encoding_encode(const h2_encoding_t *enc,
                                     const uint8_t *src, size_t src_len,
                                     char *dst, size_t dst_cap,
                                     size_t *out_len) {
    size_t need = 0;
    if (out_len == NULL || (src == NULL && src_len != 0) || (dst == NULL && dst_cap != 0) ||
        h2_encoding_max_encoded_len(enc, src_len, &need) != H2_ENCODING_OK) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    if (enc->kind == H2_ENCODING_KIND_BASE85) {
        return encode_base85(enc, src, src_len, dst, dst_cap, out_len) ? H2_ENCODING_OK
                                                                        : H2_ENCODING_ERR_NO_SPACE;
    }
    *out_len = need;
    if (need > dst_cap) {
        return H2_ENCODING_ERR_NO_SPACE;
    }
    encode_bits(enc, src, src_len, dst);
    return H2_ENCODING_OK;
}

/* ---- Decoding ---------------------------------------------------------- */

static size_t first_invalid(const uint8_t *map, const char *src, size_t len) {
    size_t i = 0;
    while (i < len && map[(unsigned char)src[i]] != INVALID_DIGIT) {
        ++i;
    }
    return i;
}

static h2_encoding_err_t corrupt_at(size_t offset, size_t *out_len) {
    *out_len = offset;
    return H2_ENCODING_ERR_CORRUPT;
}

/* Tail symbol counts that carry at least one byte more than one symbol fewer. */
static bool tail_is_complete(size_t chars, unsigned bits) {
    return chars == 0 || (chars - 1) * bits / 8 < chars * bits / 8;
}

/* Each block decoder returns the number of symbols consumed; fewer than
 * blocks * block_chars means the block holding that symbol is invalid. Valid
 * digits of a power-of-two radix never OR above radix - 1, so one test per
 * block rejects every invalid symbol. */
static size_t decode_hex_blocks(const uint8_t *map, const char *src, size_t blocks, uint8_t *out) {
    for (size_t i = 0; i < blocks; ++i) {
        unsigned hi = map[(unsigned char)src[2 * i]];
        unsigned lo = map[(unsigned char)src[2 * i + 1]];
        if ((hi | lo) > 15u) {
            return 2 * i;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 2 * blocks;
}

static size_t decode_base64_blocks(const uint8_t *map, const char *src, size_t blocks,
                                   uint8_t *out) {
    for (size_t i = 0; i < blocks; ++i, src += 4, out += 3) {
        uint32_t a = map[(unsigned char)src[0]];
        uint32_t b = map[(unsigned char)src[1]];
        uint32_t c = map[(unsigned char)src[2]];
        uint32_t d = map[(unsigned char)src[3]];
        if ((a | b | c | d) > 63u) {
            return 4 * i;
        }
        uint32_t v = a << 18 | b << 12 | c << 6 | d;
        out[0] = (uint8_t)(v >> 16);
        out[1] = (uint8_t)(v >> 8);
        out[2] = (uint8_t)v;
    }
    return 4 * blocks;
}

static size_t decode_base32_blocks(const uint8_t *map, const char *src, size_t blocks,
                                   uint8_t *out) {
    for (size_t i = 0; i < blocks; ++i, src += 8, out += 5) {
        uint32_t d[8];
        uint32_t any = 0;
        for (size_t j = 0; j < 8; ++j) {
            d[j] = map[(unsigned char)src[j]];
            any |= d[j];
        }
        if (any > 31u) {
            return 8 * i;
        }
        uint32_t hi = d[0] << 15 | d[1] << 10 | d[2] << 5 | d[3];
        uint32_t lo = d[4] << 15 | d[5] << 10 | d[6] << 5 | d[7];
        out[0] = (uint8_t)(hi >> 12);
        out[1] = (uint8_t)(hi >> 4);
        out[2] = (uint8_t)(hi << 4 | lo >> 16);
        out[3] = (uint8_t)(lo >> 8);
        out[4] = (uint8_t)lo;
    }
    return 8 * blocks;
}

static h2_encoding_err_t decode_bits(const h2_encoding_t *enc, const char *src, size_t len,
                                     uint8_t *dst, size_t cap, size_t *out_len) {
    const uint8_t *map = enc->decode_map;
    unsigned bits = k_bits[enc->kind];
    size_t block_chars = k_block_chars[enc->kind];
    size_t block_bytes = k_block_bytes[enc->kind];
    size_t data_len = len;
    if (enc->padding != '\0') {
        while (data_len > 0 && src[data_len - 1] == enc->padding) {
            --data_len;
        }
    }
    size_t tail = data_len % block_chars;
    size_t shape_error = SIZE_MAX;
    if (enc->padding != '\0') {
        size_t pad = len - data_len;
        size_t expected = tail == 0 ? 0 : block_chars - tail;
        if (pad > expected) {
            shape_error = data_len + expected;
        } else if (pad < expected) {
            shape_error = len;
        }
    }
    if (shape_error == SIZE_MAX && !tail_is_complete(tail, bits)) {
        shape_error = data_len;
    }
    if (shape_error != SIZE_MAX) {
        size_t bad = first_invalid(map, src, data_len);
        return corrupt_at(bad < data_len && bad < shape_error ? bad : shape_error, out_len);
    }
    unsigned spare = (unsigned)(tail * bits % 8);
    size_t need = data_len / block_chars * block_bytes + tail * bits / 8;
    if (need > cap) {
        size_t bad = first_invalid(map, src, data_len);
        if (bad < data_len) {
            return corrupt_at(bad, out_len);
        }
        if (spare != 0 && (map[(unsigned char)src[data_len - 1]] & ((1u << spare) - 1u)) != 0) {
            return corrupt_at(data_len - 1, out_len);
        }
        *out_len = need;
        return H2_ENCODING_ERR_NO_SPACE;
    }
    size_t blocks = data_len / block_chars;
    size_t done = enc->kind == H2_ENCODING_KIND_HEX ? decode_hex_blocks(map, src, blocks, dst)
                  : enc->kind == H2_ENCODING_KIND_BASE64
                      ? decode_base64_blocks(map, src, blocks, dst)
                      : decode_base32_blocks(map, src, blocks, dst);
    if (done < blocks * block_chars) {
        return corrupt_at(done + first_invalid(map, src + done, block_chars), out_len);
    }
    /* A base32 tail holds up to 7 symbols, 35 bits. */
    uint64_t acc = 0;
    size_t n = blocks * block_bytes;
    for (size_t i = done; i < data_len; ++i) {
        unsigned digit = map[(unsigned char)src[i]];
        if (digit == INVALID_DIGIT) {
            return corrupt_at(i, out_len);
        }
        acc = acc << bits | digit;
    }
    if ((acc & ((1u << spare) - 1u)) != 0) {
        return corrupt_at(data_len - 1, out_len);
    }
    acc >>= spare;
    for (size_t i = need - n; i > 0; --i) {
        dst[n++] = (uint8_t)(acc >> (8 * (i - 1)));
    }
    *out_len = need;
    return H2_ENCODING_OK;
}

static const uint32_t k_pow85[] = {1u, 85u, 7225u, 614125u};

/* Decodes a final group of count (2..4) digits into count - 1 bytes. The
 * encoder drops the low 5 - count digits of the zero-padded group, so the
 * payload is the unique multiple of 256^missing in
 * [prefix * 85^missing, (prefix + 1) * 85^missing). */
static bool base85_tail(uint32_t prefix, size_t count, uint32_t *payload) {
    size_t missing = 5 - count;
    uint64_t unit = (uint64_t)1 << (8 * missing);
    uint64_t low = (uint64_t)prefix * k_pow85[missing];
    uint64_t value = (low + unit - 1) / unit;
    if (value >= ((uint64_t)1 << (8 * (count - 1))) || value * unit >= low + k_pow85[missing]) {
        return false;
    }
    *payload = (uint32_t)value;
    return true;
}

/* Decodes whole groups with room already guaranteed; returns the symbols
 * consumed, stopping at the first group that is invalid or overflows. */
static size_t decode_base85_groups(const uint8_t *map, const char *src, size_t groups,
                                   uint8_t *out) {
    for (size_t i = 0; i < groups; ++i, src += 5, out += 4) {
        const unsigned char *g = (const unsigned char *)src;
        uint32_t d0 = map[g[0]], d1 = map[g[1]], d2 = map[g[2]], d3 = map[g[3]];
        uint32_t d4 = map[g[4]];
        uint32_t head = ((d0 * 85u + d1) * 85u + d2) * 85u + d3;
        if (((d0 | d1 | d2 | d3 | d4) & 0x80u) != 0 || head > BASE85_MAX_HEAD ||
            (head == BASE85_MAX_HEAD && d4 != 0)) {
            return 5 * i;
        }
        uint32_t v = head * 85u + d4;
        out[0] = (uint8_t)(v >> 24);
        out[1] = (uint8_t)(v >> 16);
        out[2] = (uint8_t)(v >> 8);
        out[3] = (uint8_t)v;
    }
    return 5 * groups;
}

static h2_encoding_err_t decode_base85(const h2_encoding_t *enc, const char *src, size_t len,
                                       uint8_t *dst, size_t cap, size_t *out_len) {
    const uint8_t *map = enc->decode_map;
    char z = enc->zero_group;
    bool writing = true;
    size_t n = 0;
    size_t i = 0;
    if (z == '\0' && len / 5 * 4 <= cap) {
        /* Without a zero group the output size follows from the length, so
         * whole groups skip the per-group capacity bookkeeping. The general
         * loop below reports any group this stops at. */
        i = decode_base85_groups(map, src, len / 5, dst);
        n = i / 5 * 4;
    }
    while (i < len) {
        if (z != '\0' && src[i] == z) {
            if (writing && cap - n >= 4) {
                memset(dst + n, 0, 4);
            } else {
                writing = false;
            }
            n += 4;
            ++i;
            continue;
        }
        if (len - i >= 5) {
            const unsigned char *g = (const unsigned char *)src + i;
            uint32_t d0 = map[g[0]], d1 = map[g[1]], d2 = map[g[2]], d3 = map[g[3]];
            uint32_t d4 = map[g[4]];
            if (((d0 | d1 | d2 | d3 | d4) & 0x80u) != 0) {
                return corrupt_at(i + first_invalid(map, src + i, 5), out_len);
            }
            uint32_t head = ((d0 * 85u + d1) * 85u + d2) * 85u + d3;
            if (head > BASE85_MAX_HEAD || (head == BASE85_MAX_HEAD && d4 != 0)) {
                return corrupt_at(i, out_len);
            }
            uint32_t v = head * 85u + d4;
            if (v == 0 && z != '\0') {
                return corrupt_at(i, out_len);
            }
            if (writing && cap - n >= 4) {
                dst[n] = (uint8_t)(v >> 24);
                dst[n + 1] = (uint8_t)(v >> 16);
                dst[n + 2] = (uint8_t)(v >> 8);
                dst[n + 3] = (uint8_t)v;
            } else {
                writing = false;
            }
            n += 4;
            i += 5;
            continue;
        }
        size_t count = len - i;
        uint32_t prefix = 0;
        for (size_t j = 0; j < count; ++j) {
            unsigned digit = map[(unsigned char)src[i + j]];
            if (digit == INVALID_DIGIT) {
                return corrupt_at(i + j, out_len);
            }
            prefix = prefix * 85u + digit;
        }
        if (count == 1) {
            return corrupt_at(len, out_len);
        }
        uint32_t payload = 0;
        if (!base85_tail(prefix, count, &payload)) {
            return corrupt_at(i, out_len);
        }
        size_t bytes = count - 1;
        if (writing && cap - n >= bytes) {
            for (size_t j = 0; j < bytes; ++j) {
                dst[n + j] = (uint8_t)(payload >> (8 * (bytes - 1 - j)));
            }
        } else {
            writing = false;
        }
        n += bytes;
        i = len;
    }
    *out_len = n;
    return writing ? H2_ENCODING_OK : H2_ENCODING_ERR_NO_SPACE;
}

h2_encoding_err_t h2_encoding_decode(const h2_encoding_t *enc,
                                     const char *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap,
                                     size_t *out_len) {
    if (out_len == NULL || (src == NULL && src_len != 0) || (dst == NULL && dst_cap != 0) ||
        !descriptor_is_usable(enc)) {
        return H2_ENCODING_ERR_INVALID_ARG;
    }
    return enc->kind == H2_ENCODING_KIND_BASE85
               ? decode_base85(enc, src, src_len, dst, dst_cap, out_len)
               : decode_bits(enc, src, src_len, dst, dst_cap, out_len);
}
