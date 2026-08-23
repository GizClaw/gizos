#include "h2_dns.h"

#include <string.h>

#define H2_DNS_RECORD_CNAME 5u

static uint16_t read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static uint8_t ascii_lower(uint8_t c) {
    if (c >= (uint8_t)'A' && c <= (uint8_t)'Z') {
        return (uint8_t)(c + ((uint8_t)'a' - (uint8_t)'A'));
    }
    return c;
}

static int skip_name(const uint8_t *packet, size_t packet_len, size_t *offset) {
    size_t pos = *offset;
    size_t jumps = 0;
    for (;;) {
        if (pos >= packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        uint8_t len = packet[pos++];
        if (len == 0u) {
            *offset = pos;
            return H2_DNS_OK;
        }
        if ((len & 0xc0u) == 0xc0u) {
            if (pos >= packet_len || ++jumps > 8u) {
                return H2_DNS_ERR_MALFORMED;
            }
            uint16_t pointer = (uint16_t)(((uint16_t)(len & 0x3fu) << 8) | packet[pos]);
            if (pointer >= packet_len) {
                return H2_DNS_ERR_MALFORMED;
            }
            *offset = pos + 1u;
            return H2_DNS_OK;
        }
        if ((len & 0xc0u) != 0u || len > 63u || pos + len > packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        pos += len;
    }
}

static int name_matches(const uint8_t *packet, size_t packet_len, size_t offset, const char *expected_name, int *out_matches) {
    if (packet == NULL || expected_name == NULL || out_matches == NULL) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    *out_matches = 0;
    size_t pos = offset;
    size_t jumps = 0u;
    const char *label = expected_name;
    for (;;) {
        if (pos >= packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        uint8_t len = packet[pos++];
        if ((len & 0xc0u) == 0xc0u) {
            if (pos >= packet_len || ++jumps > 8u) {
                return H2_DNS_ERR_MALFORMED;
            }
            uint16_t pointer = (uint16_t)(((uint16_t)(len & 0x3fu) << 8) | packet[pos++]);
            if (pointer >= packet_len) {
                return H2_DNS_ERR_MALFORMED;
            }
            pos = pointer;
            continue;
        }
        if ((len & 0xc0u) != 0u || len > 63u || pos + len > packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        if (len == 0u) {
            *out_matches = *label == '\0';
            return H2_DNS_OK;
        }
        const char *dot = strchr(label, '.');
        size_t expected_len = dot == NULL ? strlen(label) : (size_t)(dot - label);
        if (expected_len != (size_t)len) {
            return H2_DNS_OK;
        }
        for (size_t i = 0u; i < (size_t)len; ++i) {
            if (ascii_lower(packet[pos + i]) != ascii_lower((uint8_t)label[i])) {
                return H2_DNS_OK;
            }
        }
        pos += len;
        if (dot == NULL) {
            label += expected_len;
        } else {
            label = dot + 1;
        }
    }
}

static int decode_name(const uint8_t *packet, size_t packet_len, size_t offset, char *out_name, size_t out_cap) {
    if (packet == NULL || out_name == NULL || out_cap == 0u) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    size_t pos = offset;
    size_t out_len = 0u;
    size_t jumps = 0u;
    for (;;) {
        if (pos >= packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        uint8_t len = packet[pos++];
        if ((len & 0xc0u) == 0xc0u) {
            if (pos >= packet_len || ++jumps > 8u) {
                return H2_DNS_ERR_MALFORMED;
            }
            uint16_t pointer = (uint16_t)(((uint16_t)(len & 0x3fu) << 8) | packet[pos++]);
            if (pointer >= packet_len) {
                return H2_DNS_ERR_MALFORMED;
            }
            pos = pointer;
            continue;
        }
        if ((len & 0xc0u) != 0u || len > 63u || pos + len > packet_len) {
            return H2_DNS_ERR_MALFORMED;
        }
        if (len == 0u) {
            if (out_len == 0u) {
                return H2_DNS_ERR_MALFORMED;
            }
            out_name[out_len] = '\0';
            return H2_DNS_OK;
        }
        if (out_len != 0u) {
            if (out_len + 1u >= out_cap) {
                return H2_DNS_ERR_NO_SPACE;
            }
            out_name[out_len++] = '.';
        }
        if (out_len + (size_t)len >= out_cap) {
            return H2_DNS_ERR_NO_SPACE;
        }
        for (size_t i = 0u; i < (size_t)len; ++i) {
            out_name[out_len++] = (char)ascii_lower(packet[pos + i]);
        }
        pos += len;
    }
}

int h2_dns_encode_query(
    const char *name,
    h2_dns_record_type_t type,
    uint16_t txid,
    uint8_t *out_packet,
    size_t packet_cap,
    size_t *out_len) {
    if (name == NULL || out_packet == NULL || out_len == NULL ||
        (type != H2_DNS_RECORD_A && type != H2_DNS_RECORD_AAAA)) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    size_t name_len = strlen(name);
    if (name_len == 0u || name_len > H2_DNS_MAX_NAME_SIZE) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    if (packet_cap < 18u) {
        return H2_DNS_ERR_NO_SPACE;
    }
    memset(out_packet, 0, packet_cap);
    write_u16(&out_packet[0], txid);
    write_u16(&out_packet[2], 0x0100u);
    write_u16(&out_packet[4], 1u);

    size_t offset = 12u;
    const char *label = name;
    while (*label != '\0') {
        const char *dot = strchr(label, '.');
        size_t label_len = dot == NULL ? strlen(label) : (size_t)(dot - label);
        if (label_len == 0u || label_len > 63u) {
            return H2_DNS_ERR_INVALID_ARG;
        }
        if (offset + 1u + label_len + 5u > packet_cap) {
            return H2_DNS_ERR_NO_SPACE;
        }
        out_packet[offset++] = (uint8_t)label_len;
        memcpy(&out_packet[offset], label, label_len);
        offset += label_len;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    out_packet[offset++] = 0u;
    write_u16(&out_packet[offset], (uint16_t)type);
    offset += 2u;
    write_u16(&out_packet[offset], 1u);
    offset += 2u;
    *out_len = offset;
    return H2_DNS_OK;
}

int h2_dns_parse_response(
    const uint8_t *packet,
    size_t packet_len,
    uint16_t expected_txid,
    const char *expected_name,
    h2_dns_record_type_t type,
    h2_dns_answer_t *out_answers,
    size_t max_answers,
    size_t *out_count) {
    if (packet == NULL || expected_name == NULL || out_count == NULL || (out_answers == NULL && max_answers != 0u)) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    int found_answer = 0;
    if (packet_len < 12u) {
        return H2_DNS_ERR_MALFORMED;
    }
    if (read_u16(&packet[0]) != expected_txid) {
        return H2_DNS_ERR_TXID_MISMATCH;
    }
    uint16_t flags = read_u16(&packet[2]);
    if ((flags & 0x8000u) == 0u) {
        return H2_DNS_ERR_MALFORMED;
    }
    uint16_t qdcount = read_u16(&packet[4]);
    uint16_t ancount = read_u16(&packet[6]);
    if (qdcount == 0u) {
        return H2_DNS_ERR_MALFORMED;
    }
    size_t offset = 12u;
    int found_question = 0;
    for (uint16_t i = 0; i < qdcount; ++i) {
        size_t name_offset = offset;
        int rc = skip_name(packet, packet_len, &offset);
        if (rc != H2_DNS_OK) {
            return rc;
        }
        if (offset + 4u > packet_len) {
            return H2_DNS_ERR_TRUNCATED;
        }
        uint16_t question_type = read_u16(&packet[offset]);
        uint16_t question_class = read_u16(&packet[offset + 2u]);
        int question_name_matches = 0;
        rc = name_matches(packet, packet_len, name_offset, expected_name, &question_name_matches);
        if (rc != H2_DNS_OK) {
            return rc;
        }
        if (question_name_matches && question_type == (uint16_t)type && question_class == 1u) {
            found_question = 1;
        }
        offset += 4u;
    }
    if (!found_question) {
        return H2_DNS_ERR_MALFORMED;
    }
    if ((flags & 0x0200u) != 0u) {
        return H2_DNS_ERR_TRUNCATED;
    }
    uint16_t rcode = flags & 0x000fu;
    if (rcode == 3u) {
        return H2_DNS_ERR_NXDOMAIN;
    }
    if (rcode == 2u) {
        return H2_DNS_ERR_SERVER_FAILURE;
    }
    if (rcode != 0u) {
        return H2_DNS_ERR_MALFORMED;
    }
    size_t answer_offset = offset;
    char cname_target[H2_DNS_MAX_NAME_SIZE + 1u];
    cname_target[0] = '\0';
    const char *accepted_name = expected_name;
    for (uint8_t hop = 0u; hop < 8u; ++hop) {
        int found_cname = 0;
        size_t scan_offset = answer_offset;
        for (uint16_t i = 0; i < ancount; ++i) {
            size_t name_offset = scan_offset;
            int rc = skip_name(packet, packet_len, &scan_offset);
            if (rc != H2_DNS_OK) {
                return rc;
            }
            if (scan_offset + 10u > packet_len) {
                return H2_DNS_ERR_TRUNCATED;
            }
            uint16_t rr_type = read_u16(&packet[scan_offset]);
            uint16_t rr_class = read_u16(&packet[scan_offset + 2u]);
            uint16_t rdlen = read_u16(&packet[scan_offset + 8u]);
            (void)read_u32(&packet[scan_offset + 4u]);
            scan_offset += 10u;
            if (scan_offset + rdlen > packet_len) {
                return H2_DNS_ERR_TRUNCATED;
            }
            if (rr_class == 1u && rr_type == H2_DNS_RECORD_CNAME) {
                int answer_name_matches = 0;
                rc = name_matches(packet, packet_len, name_offset, accepted_name, &answer_name_matches);
                if (rc != H2_DNS_OK) {
                    return rc;
                }
                if (answer_name_matches) {
                    char next_target[H2_DNS_MAX_NAME_SIZE + 1u];
                    rc = decode_name(packet, packet_len, scan_offset, next_target, sizeof(next_target));
                    if (rc != H2_DNS_OK) {
                        return rc;
                    }
                    if (strcmp(next_target, accepted_name) == 0) {
                        return H2_DNS_ERR_MALFORMED;
                    }
                    memcpy(cname_target, next_target, strlen(next_target) + 1u);
                    accepted_name = cname_target;
                    found_cname = 1;
                    break;
                }
            }
            scan_offset += rdlen;
        }
        if (!found_cname) {
            break;
        }
        if (hop == 7u) {
            return H2_DNS_ERR_MALFORMED;
        }
    }
    offset = answer_offset;
    for (uint16_t i = 0; i < ancount; ++i) {
        size_t name_offset = offset;
        int rc = skip_name(packet, packet_len, &offset);
        if (rc != H2_DNS_OK) {
            return rc;
        }
        if (offset + 10u > packet_len) {
            return H2_DNS_ERR_TRUNCATED;
        }
        uint16_t rr_type = read_u16(&packet[offset]);
        uint16_t rr_class = read_u16(&packet[offset + 2u]);
        uint16_t rdlen = read_u16(&packet[offset + 8u]);
        (void)read_u32(&packet[offset + 4u]);
        offset += 10u;
        if (offset + rdlen > packet_len) {
            return H2_DNS_ERR_TRUNCATED;
        }
        int answer_name_matches = 0;
        rc = name_matches(packet, packet_len, name_offset, accepted_name, &answer_name_matches);
        if (rc != H2_DNS_OK) {
            return rc;
        }
        if (rr_class == 1u && rr_type == (uint16_t)type &&
            answer_name_matches &&
            ((type == H2_DNS_RECORD_A && rdlen == 4u) || (type == H2_DNS_RECORD_AAAA && rdlen == 16u))) {
            found_answer = 1;
            if (max_answers == 0u) {
                return H2_DNS_ERR_NO_SPACE;
            }
            if (*out_count >= max_answers) {
                offset += rdlen;
                continue;
            }
            h2_dns_answer_t *answer = &out_answers[*out_count];
            memset(answer, 0, sizeof(*answer));
            answer->type = type;
            answer->addr.family = type == H2_DNS_RECORD_A ? H2_PAL_NET_FAMILY_IPV4 : H2_PAL_NET_FAMILY_IPV6;
            memcpy(answer->addr.ip, &packet[offset], rdlen);
            (*out_count)++;
        }
        offset += rdlen;
    }
    return found_answer ? H2_DNS_OK : H2_DNS_ERR_NO_ANSWER;
}
