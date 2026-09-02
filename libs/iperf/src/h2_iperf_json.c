#include "h2_iperf_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- writer -------------------------------------------------------------- */

static void json_put(h2_iperf_json_writer_t *w, const char *text, size_t len) {
    if (w->overflow) {
        return;
    }
    if (w->len + len + 1u > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, text, len);
    w->len += len;
    w->buf[w->len] = '\0';
}

static void json_before_value(h2_iperf_json_writer_t *w) {
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0u && w->need_comma[w->depth - 1u]) {
        json_put(w, ",", 1u);
    }
    if (w->depth > 0u) {
        w->need_comma[w->depth - 1u] = true;
    }
}

static void json_push(h2_iperf_json_writer_t *w) {
    if (w->depth >= H2_IPERF_JSON_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    w->need_comma[w->depth] = false;
    w->depth++;
}

static void json_pop(h2_iperf_json_writer_t *w) {
    if (w->depth == 0u) {
        w->overflow = true;
        return;
    }
    w->depth--;
}

void h2_iperf_json_init(h2_iperf_json_writer_t *w, char *buf, size_t cap) {
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (cap > 0u) {
        buf[0] = '\0';
    } else {
        w->overflow = true;
    }
}

void h2_iperf_json_object_begin(h2_iperf_json_writer_t *w) {
    json_before_value(w);
    json_put(w, "{", 1u);
    json_push(w);
}

void h2_iperf_json_object_end(h2_iperf_json_writer_t *w) {
    json_pop(w);
    json_put(w, "}", 1u);
}

void h2_iperf_json_array_begin(h2_iperf_json_writer_t *w) {
    json_before_value(w);
    json_put(w, "[", 1u);
    json_push(w);
}

void h2_iperf_json_array_end(h2_iperf_json_writer_t *w) {
    json_pop(w);
    json_put(w, "]", 1u);
}

static void json_quoted(h2_iperf_json_writer_t *w, const char *text) {
    json_put(w, "\"", 1u);
    for (const char *p = text; *p != '\0'; ++p) {
        char escaped[8];
        size_t escaped_len = 0u;
        switch (*p) {
        case '"':
            memcpy(escaped, "\\\"", 2u);
            escaped_len = 2u;
            break;
        case '\\':
            memcpy(escaped, "\\\\", 2u);
            escaped_len = 2u;
            break;
        case '\n':
            memcpy(escaped, "\\n", 2u);
            escaped_len = 2u;
            break;
        case '\r':
            memcpy(escaped, "\\r", 2u);
            escaped_len = 2u;
            break;
        case '\t':
            memcpy(escaped, "\\t", 2u);
            escaped_len = 2u;
            break;
        default:
            if ((unsigned char)*p < 0x20u) {
                escaped_len = (size_t)snprintf(
                    escaped, sizeof(escaped), "\\u%04x", (unsigned)(unsigned char)*p);
            } else {
                escaped[0] = *p;
                escaped_len = 1u;
            }
            break;
        }
        json_put(w, escaped, escaped_len);
    }
    json_put(w, "\"", 1u);
}

void h2_iperf_json_key(h2_iperf_json_writer_t *w, const char *key) {
    json_before_value(w);
    json_quoted(w, key);
    json_put(w, ":", 1u);
    w->after_key = true;
}

void h2_iperf_json_bool(h2_iperf_json_writer_t *w, bool value) {
    json_before_value(w);
    if (value) {
        json_put(w, "true", 4u);
    } else {
        json_put(w, "false", 5u);
    }
}

void h2_iperf_json_i64(h2_iperf_json_writer_t *w, int64_t value) {
    char text[24];
    int n = snprintf(text, sizeof(text), "%lld", (long long)value);
    json_before_value(w);
    json_put(w, text, n > 0 ? (size_t)n : 0u);
}

void h2_iperf_json_u64(h2_iperf_json_writer_t *w, uint64_t value) {
    char text[24];
    int n = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    json_before_value(w);
    json_put(w, text, n > 0 ? (size_t)n : 0u);
}

void h2_iperf_json_f64(h2_iperf_json_writer_t *w, double value) {
    /* Six fixed decimals without relying on floating-point printf support. */
    char text[48];
    bool negative = value < 0.0;
    double magnitude = negative ? -value : value;
    if (magnitude > 9.0e15) {
        magnitude = 9.0e15;
    }
    uint64_t whole = (uint64_t)magnitude;
    double fraction = magnitude - (double)whole;
    uint64_t micro = (uint64_t)(fraction * 1000000.0 + 0.5);
    if (micro >= 1000000u) {
        micro = 0u;
        whole++;
    }
    int n = snprintf(
        text, sizeof(text), "%s%llu.%06llu",
        negative ? "-" : "",
        (unsigned long long)whole,
        (unsigned long long)micro);
    json_before_value(w);
    json_put(w, text, n > 0 ? (size_t)n : 0u);
}

void h2_iperf_json_string(h2_iperf_json_writer_t *w, const char *value) {
    json_before_value(w);
    json_quoted(w, value);
}

bool h2_iperf_json_finish(const h2_iperf_json_writer_t *w) {
    return !w->overflow && w->depth == 0u && !w->after_key && w->len > 0u;
}

/* ---- reader -------------------------------------------------------------- */

static size_t json_skip_ws(const char *json, size_t len, size_t pos) {
    while (pos < len &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' ||
            json[pos] == '\r')) {
        pos++;
    }
    return pos;
}

/* Returns the index one past the closing quote, or len when unterminated. */
static size_t json_skip_string(const char *json, size_t len, size_t pos) {
    pos++; /* opening quote */
    while (pos < len) {
        if (json[pos] == '\\') {
            pos += 2u;
            continue;
        }
        if (json[pos] == '"') {
            return pos + 1u;
        }
        pos++;
    }
    return len;
}

static size_t json_skip_value(const char *json, size_t len, size_t pos) {
    pos = json_skip_ws(json, len, pos);
    if (pos >= len) {
        return len;
    }
    if (json[pos] == '"') {
        return json_skip_string(json, len, pos);
    }
    if (json[pos] == '{' || json[pos] == '[') {
        int depth = 0;
        while (pos < len) {
            char c = json[pos];
            if (c == '"') {
                pos = json_skip_string(json, len, pos);
                continue;
            }
            if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) {
                    return pos + 1u;
                }
            }
            pos++;
        }
        return len;
    }
    while (pos < len && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != ']' && json[pos] != ' ' && json[pos] != '\n' &&
           json[pos] != '\r' && json[pos] != '\t') {
        pos++;
    }
    return pos;
}

bool h2_iperf_json_find(
    const char *json,
    size_t len,
    const char *key,
    const char **out_value,
    size_t *out_len) {
    if (json == NULL || key == NULL || out_value == NULL || out_len == NULL) {
        return false;
    }
    size_t key_len = strlen(key);
    size_t pos = 0u;
    while (pos < len) {
        char c = json[pos];
        if (c != '"') {
            pos++;
            continue;
        }
        size_t end = json_skip_string(json, len, pos);
        size_t text_len = end > pos + 2u ? end - pos - 2u : 0u;
        const char *text = json + pos + 1u;
        size_t after = json_skip_ws(json, len, end);
        bool is_key = after < len && json[after] == ':';
        if (is_key && text_len == key_len && memcmp(text, key, key_len) == 0) {
            size_t value_start = json_skip_ws(json, len, after + 1u);
            size_t value_end = json_skip_value(json, len, value_start);
            if (value_end <= value_start) {
                return false;
            }
            *out_value = json + value_start;
            *out_len = value_end - value_start;
            return true;
        }
        if (is_key) {
            /* Skip the value so nested strings cannot masquerade as keys. */
            size_t value_start = json_skip_ws(json, len, after + 1u);
            if (value_start < len && json[value_start] == '"') {
                pos = json_skip_string(json, len, value_start);
                continue;
            }
            pos = after + 1u;
            continue;
        }
        pos = end;
    }
    return false;
}

bool h2_iperf_json_parse_f64(const char *text, size_t len, double *out) {
    if (text == NULL || out == NULL || len == 0u) {
        return false;
    }
    size_t pos = 0u;
    bool negative = false;
    if (text[pos] == '-') {
        negative = true;
        pos++;
    } else if (text[pos] == '+') {
        pos++;
    }
    double value = 0.0;
    bool digits = false;
    while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10.0 + (double)(text[pos] - '0');
        pos++;
        digits = true;
    }
    if (pos < len && text[pos] == '.') {
        pos++;
        double scale = 0.1;
        while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
            value += (double)(text[pos] - '0') * scale;
            scale *= 0.1;
            pos++;
            digits = true;
        }
    }
    if (!digits) {
        return false;
    }
    if (pos < len && (text[pos] == 'e' || text[pos] == 'E')) {
        pos++;
        bool exp_negative = false;
        if (pos < len && (text[pos] == '-' || text[pos] == '+')) {
            exp_negative = text[pos] == '-';
            pos++;
        }
        int exponent = 0;
        bool exp_digits = false;
        while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
            if (exponent < 400) {
                exponent = exponent * 10 + (text[pos] - '0');
            }
            pos++;
            exp_digits = true;
        }
        if (!exp_digits) {
            return false;
        }
        while (exponent-- > 0) {
            value = exp_negative ? value / 10.0 : value * 10.0;
        }
    }
    if (pos != len) {
        return false;
    }
    *out = negative ? -value : value;
    return true;
}

bool h2_iperf_json_get_f64(const char *json, size_t len, const char *key, double *out) {
    const char *value = NULL;
    size_t value_len = 0u;
    if (!h2_iperf_json_find(json, len, key, &value, &value_len)) {
        return false;
    }
    return h2_iperf_json_parse_f64(value, value_len, out);
}

bool h2_iperf_json_get_i64(const char *json, size_t len, const char *key, int64_t *out) {
    double value = 0.0;
    if (!h2_iperf_json_get_f64(json, len, key, &value)) {
        return false;
    }
    if (value > 9.2e18 || value < -9.2e18) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

bool h2_iperf_json_get_true(const char *json, size_t len, const char *key) {
    const char *value = NULL;
    size_t value_len = 0u;
    if (!h2_iperf_json_find(json, len, key, &value, &value_len)) {
        return false;
    }
    return value_len == 4u && memcmp(value, "true", 4u) == 0;
}

bool h2_iperf_json_get_string(
    const char *json,
    size_t len,
    const char *key,
    char *out,
    size_t cap) {
    const char *value = NULL;
    size_t value_len = 0u;
    if (out == NULL || cap == 0u ||
        !h2_iperf_json_find(json, len, key, &value, &value_len)) {
        return false;
    }
    if (value_len < 2u || value[0] != '"' || value[value_len - 1u] != '"') {
        return false;
    }
    size_t out_len = 0u;
    for (size_t i = 1u; i + 1u < value_len; ++i) {
        char c = value[i];
        if (c == '\\' && i + 2u < value_len) {
            i++;
            switch (value[i]) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            default:
                c = value[i];
                break;
            }
        }
        if (out_len + 1u >= cap) {
            out[out_len] = '\0';
            return false;
        }
        out[out_len++] = c;
    }
    out[out_len] = '\0';
    return true;
}
