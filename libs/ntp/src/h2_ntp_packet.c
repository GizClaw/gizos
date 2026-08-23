#include "h2_ntp.h"

#include <string.h>

#define H2_NTP_ERA_SECONDS (UINT64_C(1) << 32)

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int timestamp_is_zero(const uint8_t *timestamp) {
    return read_u32(timestamp) == 0u && read_u32(&timestamp[4]) == 0u;
}

static int ntp_seconds_to_unix_ms(uint64_t ntp_seconds, uint32_t fraction, uint64_t *out_unix_ms) {
    if (out_unix_ms == NULL || ntp_seconds < H2_NTP_UNIX_EPOCH_DELTA) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t unix_s = ntp_seconds - H2_NTP_UNIX_EPOCH_DELTA;
    if (unix_s > UINT64_MAX / 1000u) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t ms = (((uint64_t)fraction * 1000u) + (UINT64_C(1) << 31)) >> 32;
    uint64_t unix_ms = unix_s * 1000u;
    if (UINT64_MAX - unix_ms < ms) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    *out_unix_ms = unix_ms + ms;
    return H2_NTP_OK;
}

static uint64_t abs_diff_u64(uint64_t a, uint64_t b) {
    return a >= b ? a - b : b - a;
}

static int timestamp_to_unix_ms_near(
    uint32_t seconds,
    uint32_t fraction,
    uint64_t reference_unix_ms,
    uint64_t *out_unix_ms) {
    if (out_unix_ms == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t reference_unix_s = reference_unix_ms / 1000u;
    if (UINT64_MAX - reference_unix_s < H2_NTP_UNIX_EPOCH_DELTA) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t reference_ntp_s = reference_unix_s + H2_NTP_UNIX_EPOCH_DELTA;
    uint64_t base_era = reference_ntp_s / H2_NTP_ERA_SECONDS;
    uint64_t first_era = base_era == 0u ? 0u : base_era - 1u;
    uint64_t last_era = base_era == UINT64_MAX ? UINT64_MAX : base_era + 1u;
    int found = 0;
    uint64_t best_ms = 0u;
    uint64_t best_diff = UINT64_MAX;
    for (uint64_t era = first_era; era <= last_era; ++era) {
        if (era > UINT64_MAX / H2_NTP_ERA_SECONDS) {
            break;
        }
        uint64_t era_base = era * H2_NTP_ERA_SECONDS;
        if (UINT64_MAX - era_base < (uint64_t)seconds) {
            break;
        }
        uint64_t candidate_ms = 0u;
        int rc = ntp_seconds_to_unix_ms(era_base + (uint64_t)seconds, fraction, &candidate_ms);
        if (rc != H2_NTP_OK) {
            continue;
        }
        uint64_t diff = abs_diff_u64(candidate_ms, reference_unix_ms);
        if (!found || diff < best_diff) {
            found = 1;
            best_ms = candidate_ms;
            best_diff = diff;
        }
        if (era == last_era) {
            break;
        }
    }
    if (!found) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    *out_unix_ms = best_ms;
    return H2_NTP_OK;
}

int h2_ntp_unix_ms_to_timestamp(uint64_t unix_ms, uint32_t *out_seconds, uint32_t *out_fraction) {
    if (out_seconds == NULL || out_fraction == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t unix_s = unix_ms / 1000u;
    uint64_t ms = unix_ms % 1000u;
    if (UINT64_MAX - unix_s < H2_NTP_UNIX_EPOCH_DELTA) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t ntp_s = unix_s + H2_NTP_UNIX_EPOCH_DELTA;
    *out_seconds = (uint32_t)(ntp_s & UINT32_MAX);
    *out_fraction = (uint32_t)((ms << 32) / 1000u);
    return H2_NTP_OK;
}

int h2_ntp_timestamp_to_unix_ms(uint32_t seconds, uint32_t fraction, uint64_t *out_unix_ms) {
    if (out_unix_ms == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t era = seconds < H2_NTP_UNIX_EPOCH_DELTA ? 1u : 0u;
    return ntp_seconds_to_unix_ms(era * H2_NTP_ERA_SECONDS + (uint64_t)seconds, fraction, out_unix_ms);
}

int h2_ntp_build_request(uint64_t unix_ms, uint8_t out_packet[H2_NTP_PACKET_SIZE]) {
    if (out_packet == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    memset(out_packet, 0, H2_NTP_PACKET_SIZE);
    out_packet[0] = 0x23u;
    uint32_t seconds = 0u;
    uint32_t fraction = 0u;
    int rc = h2_ntp_unix_ms_to_timestamp(unix_ms, &seconds, &fraction);
    if (rc != H2_NTP_OK) {
        return rc;
    }
    write_u32(&out_packet[40], seconds);
    write_u32(&out_packet[44], fraction);
    return H2_NTP_OK;
}

int h2_ntp_parse_response(
    const uint8_t packet[H2_NTP_PACKET_SIZE],
    uint64_t expected_originate_unix_ms,
    uint64_t local_receive_wall_ms,
    uint64_t local_receive_monotonic_ms,
    h2_ntp_sync_result_t *out_result) {
    if (packet == NULL || out_result == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    uint64_t originate_ms = 0u;
    uint64_t receive_ms = 0u;
    uint64_t transmit_ms = 0u;
    int rc = timestamp_to_unix_ms_near(
        read_u32(&packet[24]),
        read_u32(&packet[28]),
        expected_originate_unix_ms,
        &originate_ms);
    if (rc != H2_NTP_OK) {
        return H2_NTP_ERR_MALFORMED;
    }
    if (originate_ms != expected_originate_unix_ms) {
        return H2_NTP_ERR_TXID_MISMATCH;
    }
    uint8_t leap_indicator = (uint8_t)(packet[0] >> 6);
    uint8_t mode = packet[0] & 0x07u;
    uint8_t stratum = packet[1];
    if (leap_indicator == 3u || mode != 4u || stratum == 0u || stratum >= 16u) {
        return H2_NTP_ERR_UNSYNCED;
    }
    if (timestamp_is_zero(&packet[32]) || timestamp_is_zero(&packet[40])) {
        return H2_NTP_ERR_MALFORMED;
    }
    rc = timestamp_to_unix_ms_near(
        read_u32(&packet[32]),
        read_u32(&packet[36]),
        local_receive_wall_ms,
        &receive_ms);
    if (rc != H2_NTP_OK) {
        return H2_NTP_ERR_MALFORMED;
    }
    rc = timestamp_to_unix_ms_near(
        read_u32(&packet[40]),
        read_u32(&packet[44]),
        local_receive_wall_ms,
        &transmit_ms);
    if (rc != H2_NTP_OK) {
        return H2_NTP_ERR_MALFORMED;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->server_unix_ms = transmit_ms;
    out_result->local_receive_monotonic_ms = local_receive_monotonic_ms;
    int64_t t1 = (int64_t)expected_originate_unix_ms;
    int64_t t2 = (int64_t)receive_ms;
    int64_t t3 = (int64_t)transmit_ms;
    int64_t t4 = (int64_t)local_receive_wall_ms;
    int64_t round_trip_ms = (t4 - t1) - (t3 - t2);
    if (round_trip_ms < 0) {
        round_trip_ms = 0;
    }
    out_result->round_trip_ms = round_trip_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)round_trip_ms;
    out_result->offset_ms = ((t2 - t1) + (t3 - t4)) / 2;
    return H2_NTP_OK;
}
