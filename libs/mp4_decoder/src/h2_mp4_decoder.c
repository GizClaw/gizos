#include "h2_mp4_decoder.h"
#include "h2_mp4_decoder_sizing.h"

#include <limits.h>
#include <string.h>

#define BOX(a, b, c, d) \
    (((uint32_t)(a) << 24u) | ((uint32_t)(b) << 16u) | ((uint32_t)(c) << 8u) | (uint32_t)(d))

typedef struct mp4_sample {
    uint64_t offset;
    uint32_t size;
    int64_t dts_us;
    int64_t pts_us;
    int64_t duration_us;
    int sync;
} mp4_sample_t;

typedef struct mp4_track {
    uint32_t kind;
    uint32_t timescale;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t nal_length_size;
    const uint8_t *codec_config;
    size_t codec_config_size;
    mp4_sample_t *samples;
    size_t sample_count;
    int64_t duration_us;
} mp4_track_t;

typedef struct mp4_box {
    uint32_t type;
    size_t start;
    size_t payload;
    size_t end;
} mp4_box_t;

struct h2_mp4_decoder_frame {
    struct h2_mp4_decoder *owner;
};

struct h2_mp4_decoder {
    h2_pal_mem_api_t allocator;
    h2_pal_video_decoder_api_t video_api;
    uint8_t *file;
    size_t file_size;
    mp4_track_t video;
    mp4_track_t audio;
    uint8_t *video_codec_config;
    size_t video_codec_config_size;
    h2_pal_video_decoder_session_t *video_session;
    uint8_t *packet;
    size_t packet_capacity;
    uint8_t *video_storage;
    size_t video_storage_capacity;
    size_t video_storage_limit;
    int16_t *pcm_all;
    size_t pcm_all_samples;
    size_t pcm_all_capacity;
    size_t pcm_all_limit;
    int64_t pcm_origin_sample;
    int pcm_has_origin;
    int16_t *pcm_frame;
    size_t pcm_frame_capacity;
    h2_mp4_decoder_info_t info;
    h2_mp4_decoder_frame_info_t frame_info;
    h2_mp4_decoder_frame_t frame;
    h2_video_pixel_format_t video_format;
    size_t video_sample_index;
    int64_t audio_cursor_sample;
    int video_eos_submitted;
    int acquired;
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
        ((uint32_t)p[2] << 8u) | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32u) | be32(p + 4u);
}

static int parse_box(const uint8_t *data, size_t limit, size_t at, mp4_box_t *box) {
    if (at > limit || limit - at < 8u) {
        return 0;
    }
    uint64_t size = be32(data + at);
    size_t header = 8u;
    if (size == 1u) {
        if (limit - at < 16u) {
            return 0;
        }
        size = be64(data + at + 8u);
        header = 16u;
    } else if (size == 0u) {
        size = limit - at;
    }
    if (size < header || size > SIZE_MAX || (size_t)size > limit - at) {
        return 0;
    }
    box->type = be32(data + at + 4u);
    box->start = at;
    box->payload = at + header;
    box->end = at + (size_t)size;
    return 1;
}

static int find_child(
    const uint8_t *data,
    size_t begin,
    size_t end,
    uint32_t type,
    mp4_box_t *out) {
    size_t at = begin;
    while (at < end) {
        mp4_box_t box;
        if (!parse_box(data, end, at, &box)) {
            return 0;
        }
        if (box.type == type) {
            *out = box;
            return 1;
        }
        at = box.end;
    }
    return 0;
}

static int checked_add_i64(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return 0;
    }
    *out = left + right;
    return 1;
}

static int checked_mul_i64_u32(
    int64_t value,
    uint32_t factor,
    int64_t *out) {
    if (factor != 0u &&
        (value > INT64_MAX / (int64_t)factor ||
         value < INT64_MIN / (int64_t)factor)) {
        return 0;
    }
    *out = value * (int64_t)factor;
    return 1;
}

static int checked_i64_span_size(
    int64_t start,
    int64_t end,
    size_t *out) {
    if (end < start) {
        return 0;
    }
    const uint64_t sign_bit = UINT64_C(1) << 63u;
    const uint64_t ordered_start = (uint64_t)start ^ sign_bit;
    const uint64_t ordered_end = (uint64_t)end ^ sign_bit;
    const uint64_t span = ordered_end - ordered_start;
    if (span > SIZE_MAX) {
        return 0;
    }
    *out = (size_t)span;
    return 1;
}

static int ceil_duration_samples(
    int64_t duration_us,
    uint32_t sample_rate_hz,
    size_t *out_samples) {
    if (duration_us < 0 || sample_rate_hz == 0u) {
        return 0;
    }
    const uint64_t duration = (uint64_t)duration_us;
    const uint64_t whole_seconds = duration / UINT64_C(1000000);
    const uint64_t remainder_us = duration % UINT64_C(1000000);
    if (whole_seconds > SIZE_MAX / sample_rate_hz) {
        return 0;
    }
    size_t samples = (size_t)whole_seconds * sample_rate_hz;
    const uint64_t remainder_product =
        remainder_us * (uint64_t)sample_rate_hz;
    const uint64_t fractional =
        (remainder_product + UINT64_C(999999)) / UINT64_C(1000000);
    if (fractional > SIZE_MAX - samples) {
        return 0;
    }
    samples += (size_t)fractional;
    *out_samples = samples;
    return 1;
}

static int64_t scale_us(int64_t value, uint32_t timescale) {
    if (timescale == 0u) {
        return INT64_MIN;
    }
    const int64_t divisor = (int64_t)timescale;
    const int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    int64_t whole = 0;
    if (!checked_mul_i64_u32(quotient, 1000000u, &whole)) {
        return INT64_MIN;
    }
    const int64_t fractional =
        (remainder * INT64_C(1000000)) / divisor;
    int64_t result = 0;
    return checked_add_i64(whole, fractional, &result)
        ? result
        : INT64_MIN;
}

static int checked_mul_size(size_t left, size_t right, size_t *out) {
    if (right != 0u && left > SIZE_MAX / right) {
        return 0;
    }
    *out = left * right;
    return 1;
}

static int timestamp_to_signed_sample(
    int64_t timestamp_us,
    uint32_t sample_rate_hz,
    int64_t *out_sample) {
    const int64_t quotient = timestamp_us / INT64_C(1000000);
    const int64_t remainder = timestamp_us % INT64_C(1000000);
    int64_t whole = 0;
    if (!checked_mul_i64_u32(quotient, sample_rate_hz, &whole)) {
        return 0;
    }
    const int64_t remainder_product =
        remainder * (int64_t)sample_rate_hz;
    const int64_t fractional = remainder_product >= 0
        ? remainder_product / INT64_C(1000000)
        : -((-remainder_product + INT64_C(999999)) /
            INT64_C(1000000));
    return checked_add_i64(whole, fractional, out_sample);
}

static int pcm_timeline_end(
    const h2_mp4_decoder_t *decoder,
    int64_t *out_end) {
    if (!decoder->pcm_has_origin ||
        decoder->audio.channels == 0u ||
        (decoder->pcm_all_samples % decoder->audio.channels) != 0u) {
        return 0;
    }
    const size_t length =
        decoder->pcm_all_samples / decoder->audio.channels;
#if SIZE_MAX > INT64_MAX
    if (length > (size_t)INT64_MAX) {
        return 0;
    }
#endif
    if (decoder->pcm_origin_sample > INT64_MAX - (int64_t)length) {
        return 0;
    }
    *out_end = decoder->pcm_origin_sample + (int64_t)length;
    return 1;
}

static h2_pal_result_t reserve_pcm_all(
    h2_mp4_decoder_t *decoder,
    size_t required) {
    if (required <= decoder->pcm_all_capacity) {
        return H2_PAL_OK;
    }
    if (required > decoder->pcm_all_limit) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t capacity = decoder->pcm_all_capacity;
    if (capacity == 0u) {
        capacity = 1u;
    }
    while (capacity < required) {
        if (capacity > decoder->pcm_all_limit / 2u) {
            capacity = decoder->pcm_all_limit;
            break;
        }
        capacity *= 2u;
    }
    size_t bytes = 0u;
    if (capacity < required ||
        !checked_mul_size(capacity, sizeof(int16_t), &bytes)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    int16_t *replacement =
        h2_pal_mem_alloc(&decoder->allocator, bytes);
    if (replacement == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (decoder->pcm_all_samples != 0u) {
        memcpy(
            replacement,
            decoder->pcm_all,
            decoder->pcm_all_samples * sizeof(int16_t));
    }
    h2_pal_mem_free(&decoder->allocator, decoder->pcm_all);
    decoder->pcm_all = replacement;
    decoder->pcm_all_capacity = capacity;
    return H2_PAL_OK;
}

static h2_pal_result_t reserve_video_storage(
    h2_mp4_decoder_t *decoder,
    size_t required) {
    if (required <= decoder->video_storage_capacity) {
        return H2_PAL_OK;
    }
    if (required > decoder->video_storage_limit) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t capacity = decoder->video_storage_capacity;
    if (capacity == 0u) {
        capacity = 1u;
    }
    while (capacity < required) {
        if (capacity > decoder->video_storage_limit / 2u) {
            capacity = decoder->video_storage_limit;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < required) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    uint8_t *replacement =
        h2_pal_mem_alloc(&decoder->allocator, capacity);
    if (replacement == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_mem_free(&decoder->allocator, decoder->video_storage);
    decoder->video_storage = replacement;
    decoder->video_storage_capacity = capacity;
    return H2_PAL_OK;
}

static int parse_mdhd(const uint8_t *data, const mp4_box_t *box, uint32_t *timescale) {
    if (box->end - box->payload < 20u) {
        return 0;
    }
    const uint8_t version = data[box->payload];
    const size_t offset = version == 1u ? 20u : 12u;
    if ((version != 0u && version != 1u) || box->end - box->payload < offset + 4u) {
        return 0;
    }
    *timescale = be32(data + box->payload + offset);
    return *timescale != 0u;
}

static int parse_esds_asc(
    const uint8_t *data,
    size_t begin,
    size_t end,
    const uint8_t **asc,
    size_t *asc_size) {
    for (size_t i = begin; i + 2u <= end; ++i) {
        if (data[i] != 0x05u) {
            continue;
        }
        size_t at = i + 1u;
        size_t length = 0u;
        for (unsigned n = 0u; n < 4u && at < end; ++n) {
            const uint8_t byte = data[at++];
            if (length > (SIZE_MAX >> 7u)) {
                return 0;
            }
            length = (length << 7u) | (byte & 0x7fu);
            if ((byte & 0x80u) == 0u) {
                if (length >= 2u && length <= 16u && length <= end - at &&
                    (data[at] >> 3u) == 2u) {
                    *asc = data + at;
                    *asc_size = length;
                    return 1;
                }
                break;
            }
        }
    }
    return 0;
}

typedef struct table_view {
    mp4_box_t stts;
    mp4_box_t ctts;
    mp4_box_t stsc;
    mp4_box_t stsz;
    mp4_box_t stco;
    mp4_box_t stss;
    int has_ctts;
    int has_stss;
    int compact_sizes;
    int co64;
} table_view_t;

static int table_entry_count(const uint8_t *data, const mp4_box_t *box, uint32_t *count) {
    if (box->end - box->payload < 8u) {
        return 0;
    }
    *count = be32(data + box->payload + 4u);
    return 1;
}

static int build_samples(
    h2_mp4_decoder_t *decoder,
    mp4_track_t *track,
    const table_view_t *tables,
    size_t max_samples) {
    const uint8_t *data = decoder->file;
    const size_t size_table_bytes = tables->stsz.end - tables->stsz.payload;
    if (size_table_bytes < 12u) {
        return 0;
    }
    const uint8_t compact_field_size =
        tables->compact_sizes ? data[tables->stsz.payload + 7u] : 0u;
    const uint32_t fixed_size =
        tables->compact_sizes ? 0u : be32(data + tables->stsz.payload + 4u);
    const uint32_t sample_count32 = be32(data + tables->stsz.payload + 8u);
    const size_t sample_count = sample_count32;
    size_t compact_bytes = 0u;
    if (tables->compact_sizes) {
        if (compact_field_size != 4u && compact_field_size != 8u &&
            compact_field_size != 16u) {
            return 0;
        }
        if (compact_field_size == 4u) {
            compact_bytes = sample_count / 2u + sample_count % 2u;
        } else if (compact_field_size == 8u) {
            compact_bytes = sample_count;
        } else {
            if (sample_count > SIZE_MAX / 2u) {
                return 0;
            }
            compact_bytes = sample_count * 2u;
        }
    }
    if (sample_count == 0u || sample_count > max_samples ||
        (!tables->compact_sizes && fixed_size == 0u &&
         sample_count > (size_table_bytes - 12u) / 4u) ||
        (tables->compact_sizes && compact_bytes > size_table_bytes - 12u) ||
        sample_count > SIZE_MAX / sizeof(mp4_sample_t)) {
        return 0;
    }
    mp4_sample_t *samples =
        h2_pal_mem_alloc(&decoder->allocator, sample_count * sizeof(*samples));
    if (samples == NULL) {
        return 0;
    }
    memset(samples, 0, sample_count * sizeof(*samples));
    for (size_t i = 0; i < sample_count; ++i) {
        if (!tables->compact_sizes) {
            samples[i].size = fixed_size != 0u
                ? fixed_size
                : be32(data + tables->stsz.payload + 12u + i * 4u);
        } else if (compact_field_size == 4u) {
            const uint8_t packed = data[tables->stsz.payload + 12u + i / 2u];
            samples[i].size = (i & 1u) == 0u ? packed >> 4u : packed & 0x0fu;
        } else if (compact_field_size == 8u) {
            samples[i].size = data[tables->stsz.payload + 12u + i];
        } else {
            samples[i].size = be16(data + tables->stsz.payload + 12u + i * 2u);
        }
        samples[i].sync = !tables->has_stss;
    }

    uint32_t chunk_count32 = 0u;
    uint32_t stsc_count32 = 0u;
    if (!table_entry_count(data, &tables->stco, &chunk_count32) ||
        !table_entry_count(data, &tables->stsc, &stsc_count32)) {
        h2_pal_mem_free(&decoder->allocator, samples);
        return 0;
    }
    const size_t chunk_count = chunk_count32;
    const size_t chunk_stride = tables->co64 ? 8u : 4u;
    if (chunk_count == 0u || chunk_count > sample_count ||
        chunk_count > (tables->stco.end - tables->stco.payload - 8u) / chunk_stride ||
        (size_t)stsc_count32 > (tables->stsc.end - tables->stsc.payload - 8u) / 12u ||
        stsc_count32 == 0u) {
        h2_pal_mem_free(&decoder->allocator, samples);
        return 0;
    }
    size_t sample_index = 0u;
    uint32_t stsc_index = 0u;
    for (size_t chunk_index = 0u; chunk_index < chunk_count; ++chunk_index) {
        const uint32_t chunk = (uint32_t)(chunk_index + 1u);
        while (stsc_index + 1u < stsc_count32 &&
               be32(data + tables->stsc.payload + 8u + (stsc_index + 1u) * 12u) <= chunk) {
            ++stsc_index;
        }
        const uint8_t *entry = data + tables->stsc.payload + 8u + stsc_index * 12u;
        if (be32(entry) > chunk) {
            h2_pal_mem_free(&decoder->allocator, samples);
            return 0;
        }
        const uint32_t per_chunk = be32(entry + 4u);
        if (per_chunk == 0u) {
            h2_pal_mem_free(&decoder->allocator, samples);
            return 0;
        }
        uint64_t offset = tables->co64
            ? be64(data + tables->stco.payload + 8u + chunk_index * 8u)
            : be32(data + tables->stco.payload + 8u + chunk_index * 4u);
        for (uint32_t j = 0u; j < per_chunk; ++j) {
            if (sample_index >= sample_count || offset > decoder->file_size ||
                samples[sample_index].size > decoder->file_size - offset) {
                h2_pal_mem_free(&decoder->allocator, samples);
                return 0;
            }
            samples[sample_index].offset = offset;
            offset += samples[sample_index].size;
            ++sample_index;
        }
    }
    if (sample_index != sample_count) {
        h2_pal_mem_free(&decoder->allocator, samples);
        return 0;
    }

    uint32_t stts_count32 = 0u;
    if (!table_entry_count(data, &tables->stts, &stts_count32) ||
        (size_t)stts_count32 > (tables->stts.end - tables->stts.payload - 8u) / 8u) {
        h2_pal_mem_free(&decoder->allocator, samples);
        return 0;
    }
    sample_index = 0u;
    int64_t dts = 0;
    for (uint32_t i = 0u; i < stts_count32; ++i) {
        const uint8_t *entry = data + tables->stts.payload + 8u + i * 8u;
        const uint32_t count = be32(entry);
        const uint32_t delta = be32(entry + 4u);
        for (uint32_t j = 0u; j < count; ++j) {
            if (sample_index >= sample_count || dts > INT64_MAX - delta) {
                h2_pal_mem_free(&decoder->allocator, samples);
                return 0;
            }
            samples[sample_index].dts_us = scale_us(dts, track->timescale);
            samples[sample_index].pts_us = samples[sample_index].dts_us;
            samples[sample_index].duration_us = scale_us(delta, track->timescale);
            dts += delta;
            ++sample_index;
        }
    }
    if (sample_index != sample_count) {
        h2_pal_mem_free(&decoder->allocator, samples);
        return 0;
    }

    if (tables->has_ctts) {
        uint32_t count32 = 0u;
        if (!table_entry_count(data, &tables->ctts, &count32) ||
            (size_t)count32 > (tables->ctts.end - tables->ctts.payload - 8u) / 8u) {
            h2_pal_mem_free(&decoder->allocator, samples);
            return 0;
        }
        const uint8_t version = data[tables->ctts.payload];
        sample_index = 0u;
        for (uint32_t i = 0u; i < count32; ++i) {
            const uint8_t *entry = data + tables->ctts.payload + 8u + i * 8u;
            const uint32_t count = be32(entry);
            const int64_t offset = version == 1u
                ? (int64_t)(int32_t)be32(entry + 4u)
                : (int64_t)be32(entry + 4u);
            const int64_t offset_us = scale_us(offset, track->timescale);
            for (uint32_t j = 0u; j < count; ++j) {
                if (sample_index >= sample_count || offset_us == INT64_MIN ||
                    (offset_us > 0 && samples[sample_index].dts_us > INT64_MAX - offset_us) ||
                    (offset_us < 0 && samples[sample_index].dts_us < INT64_MIN - offset_us)) {
                    h2_pal_mem_free(&decoder->allocator, samples);
                    return 0;
                }
                samples[sample_index].pts_us = samples[sample_index].dts_us + offset_us;
                ++sample_index;
            }
        }
        if (sample_index != sample_count) {
            h2_pal_mem_free(&decoder->allocator, samples);
            return 0;
        }
    }

    if (tables->has_stss) {
        uint32_t count32 = 0u;
        if (!table_entry_count(data, &tables->stss, &count32) ||
            (size_t)count32 > (tables->stss.end - tables->stss.payload - 8u) / 4u) {
            h2_pal_mem_free(&decoder->allocator, samples);
            return 0;
        }
        for (uint32_t i = 0u; i < count32; ++i) {
            const uint32_t number = be32(data + tables->stss.payload + 8u + i * 4u);
            if (number == 0u || number > sample_count) {
                h2_pal_mem_free(&decoder->allocator, samples);
                return 0;
            }
            samples[number - 1u].sync = 1;
        }
    }
    track->samples = samples;
    track->sample_count = sample_count;
    track->duration_us = scale_us(dts, track->timescale);
    return track->duration_us != INT64_MIN;
}

static int parse_track(
    h2_mp4_decoder_t *decoder,
    const mp4_box_t *trak,
    mp4_track_t *track,
    size_t max_samples) {
    const uint8_t *data = decoder->file;
    mp4_box_t mdia, mdhd, hdlr, minf, stbl, stsd;
    if (!find_child(data, trak->payload, trak->end, BOX('m','d','i','a'), &mdia) ||
        !find_child(data, mdia.payload, mdia.end, BOX('m','d','h','d'), &mdhd) ||
        !find_child(data, mdia.payload, mdia.end, BOX('h','d','l','r'), &hdlr) ||
        !find_child(data, mdia.payload, mdia.end, BOX('m','i','n','f'), &minf) ||
        !find_child(data, minf.payload, minf.end, BOX('s','t','b','l'), &stbl) ||
        !find_child(data, stbl.payload, stbl.end, BOX('s','t','s','d'), &stsd) ||
        !parse_mdhd(data, &mdhd, &track->timescale) ||
        hdlr.end - hdlr.payload < 12u || stsd.end - stsd.payload < 16u) {
        return 0;
    }
    track->kind = be32(data + hdlr.payload + 8u);
    const size_t entry_at = stsd.payload + 8u;
    mp4_box_t entry;
    if (!parse_box(data, stsd.end, entry_at, &entry)) {
        return 0;
    }
    if (track->kind == BOX('v','i','d','e')) {
        if (entry.type != BOX('a','v','c','1') || entry.end - entry.start < 86u) {
            return 0;
        }
        track->width = be16(data + entry.start + 32u);
        track->height = be16(data + entry.start + 34u);
        mp4_box_t avcc;
        if (track->width == 0u || track->height == 0u ||
            !find_child(data, entry.start + 86u, entry.end, BOX('a','v','c','C'), &avcc) ||
            avcc.end - avcc.payload < 7u) {
            return 0;
        }
        track->codec_config = data + avcc.payload;
        track->codec_config_size = avcc.end - avcc.payload;
        track->nal_length_size = (uint8_t)((data[avcc.payload + 4u] & 3u) + 1u);
    } else if (track->kind == BOX('s','o','u','n')) {
        if (entry.type != BOX('m','p','4','a') || entry.end - entry.start < 36u) {
            return 0;
        }
        const uint16_t channel_count = be16(data + entry.start + 24u);
        track->sample_rate = be32(data + entry.start + 32u) >> 16u;
        mp4_box_t esds;
        if (channel_count == 0u || channel_count > UINT8_MAX ||
            track->sample_rate == 0u ||
            !find_child(data, entry.start + 36u, entry.end, BOX('e','s','d','s'), &esds) ||
            !parse_esds_asc(
                data, esds.payload + 4u, esds.end,
                &track->codec_config, &track->codec_config_size)) {
            return 0;
        }
        track->channels = (uint8_t)channel_count;
    } else {
        return 0;
    }

    table_view_t tables = {0};
    if (!find_child(data, stbl.payload, stbl.end, BOX('s','t','t','s'), &tables.stts) ||
        !find_child(data, stbl.payload, stbl.end, BOX('s','t','s','c'), &tables.stsc)) {
        return 0;
    }
    if (!find_child(data, stbl.payload, stbl.end, BOX('s','t','s','z'), &tables.stsz)) {
        if (!find_child(data, stbl.payload, stbl.end, BOX('s','t','z','2'), &tables.stsz)) {
            return 0;
        }
        tables.compact_sizes = 1;
    }
    tables.has_ctts =
        find_child(data, stbl.payload, stbl.end, BOX('c','t','t','s'), &tables.ctts);
    tables.has_stss =
        find_child(data, stbl.payload, stbl.end, BOX('s','t','s','s'), &tables.stss);
    if (!find_child(data, stbl.payload, stbl.end, BOX('s','t','c','o'), &tables.stco)) {
        if (!find_child(data, stbl.payload, stbl.end, BOX('c','o','6','4'), &tables.stco)) {
            return 0;
        }
        tables.co64 = 1;
    }
    return build_samples(decoder, track, &tables, max_samples);
}

static int build_annex_b_config(h2_mp4_decoder_t *decoder) {
    const uint8_t *avcc = decoder->video.codec_config;
    const size_t size = decoder->video.codec_config_size;
    if (size < 7u || avcc[0] != 1u) {
        return 0;
    }
    size_t at = 6u;
    size_t required = 0u;
    const uint8_t sps_count = avcc[5] & 0x1fu;
    for (unsigned phase = 0u; phase < 2u; ++phase) {
        unsigned count = phase == 0u ? sps_count : (at < size ? avcc[at++] : 0u);
        if (count == 0u) {
            return 0;
        }
        for (unsigned i = 0u; i < count; ++i) {
            if (size - at < 2u) {
                return 0;
            }
            const size_t length = be16(avcc + at);
            at += 2u;
            if (length == 0u || length > size - at || required > SIZE_MAX - length - 4u) {
                return 0;
            }
            required += length + 4u;
            at += length;
        }
    }
    uint8_t *output = h2_pal_mem_alloc(&decoder->allocator, required);
    if (output == NULL) {
        return 0;
    }
    at = 6u;
    size_t out = 0u;
    for (unsigned phase = 0u; phase < 2u; ++phase) {
        const unsigned count = phase == 0u ? sps_count : avcc[at++];
        for (unsigned i = 0u; i < count; ++i) {
            const size_t length = be16(avcc + at);
            at += 2u;
            output[out++] = 0u;
            output[out++] = 0u;
            output[out++] = 0u;
            output[out++] = 1u;
            memcpy(output + out, avcc + at, length);
            out += length;
            at += length;
        }
    }
    decoder->video_codec_config = output;
    decoder->video_codec_config_size = out;
    return 1;
}

static h2_pal_result_t configure_video(h2_mp4_decoder_t *decoder) {
    const h2_video_decoder_config_t open_config = {
        .frame_allocator = &decoder->allocator,
        .preferred_format = decoder->video_format,
    };
    h2_pal_result_t result =
        h2_pal_video_decoder_open(&decoder->video_api, &open_config, &decoder->video_session);
    if (result != H2_PAL_OK) {
        return result;
    }
    const h2_video_decoder_stream_config_t stream = {
        .codec = H2_VIDEO_CODEC_H264,
        .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
        .coded_width = decoder->video.width,
        .coded_height = decoder->video.height,
        .visible_width = decoder->video.width,
        .visible_height = decoder->video.height,
        .codec_config = decoder->video_codec_config,
        .codec_config_size = decoder->video_codec_config_size,
    };
    result = h2_pal_video_decoder_configure(&decoder->video_api, decoder->video_session, &stream);
    if (result != H2_PAL_OK) {
        (void)h2_pal_video_decoder_close(&decoder->video_api, decoder->video_session);
        decoder->video_session = NULL;
    }
    return result;
}

static h2_pal_result_t append_audio_frame(
    h2_mp4_decoder_t *decoder,
    const h2_pal_audio_decoder_api_t *api,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
    h2_audio_decoder_frame_info_t info = {0};
    h2_pal_result_t result =
        h2_pal_audio_decoder_frame_get_info(api, session, frame, &info);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (info.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        info.sample_rate_hz != decoder->audio.sample_rate ||
        info.channels != decoder->audio.channels ||
        info.data == NULL || (info.bytes % sizeof(int16_t)) != 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t values = info.bytes / sizeof(int16_t);
    size_t expected_values = 0u;
    if (info.samples_per_channel == 0u ||
        !checked_mul_size(
            info.samples_per_channel, info.channels, &expected_values) ||
        expected_values != values) {
        return H2_PAL_ERR_FORMAT;
    }
    int64_t start = 0;
    if (!timestamp_to_signed_sample(
            info.pts_us, info.sample_rate_hz, &start) ||
        start > INT64_MAX - (int64_t)info.samples_per_channel) {
        return H2_PAL_ERR_FORMAT;
    }
    const int64_t end = start + (int64_t)info.samples_per_channel;
    if (!decoder->pcm_has_origin) {
        const h2_pal_result_t reserve =
            reserve_pcm_all(decoder, values);
        if (reserve != H2_PAL_OK) {
            return reserve;
        }
        memcpy(decoder->pcm_all, info.data, info.bytes);
        decoder->pcm_all_samples = values;
        decoder->pcm_origin_sample = start;
        decoder->pcm_has_origin = 1;
        return H2_PAL_OK;
    }

    int64_t current_end = 0;
    if (!pcm_timeline_end(decoder, &current_end)) {
        return H2_PAL_ERR_FORMAT;
    }
    const int64_t new_origin =
        start < decoder->pcm_origin_sample
        ? start
        : decoder->pcm_origin_sample;
    const int64_t new_end = end > current_end ? end : current_end;
    size_t new_length = 0u;
    if (!checked_i64_span_size(new_origin, new_end, &new_length) ||
        new_length == 0u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t required_values = 0u;
    if (!checked_mul_size(
            new_length, decoder->audio.channels, &required_values)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    result = reserve_pcm_all(decoder, required_values);
    if (result != H2_PAL_OK) {
        return result;
    }

    const size_t old_length =
        decoder->pcm_all_samples / decoder->audio.channels;
    const size_t old_offset =
        (size_t)(decoder->pcm_origin_sample - new_origin);
    if (old_offset != 0u) {
        memmove(
            decoder->pcm_all + old_offset * decoder->audio.channels,
            decoder->pcm_all,
            decoder->pcm_all_samples * sizeof(int16_t));
        memset(
            decoder->pcm_all,
            0,
            old_offset * decoder->audio.channels * sizeof(int16_t));
    }
    const size_t old_end_offset = old_offset + old_length;
    if (new_length > old_end_offset) {
        memset(
            decoder->pcm_all +
                old_end_offset * decoder->audio.channels,
            0,
            (new_length - old_end_offset) *
                decoder->audio.channels * sizeof(int16_t));
    }
    const size_t input_offset = (size_t)(start - new_origin);
    memcpy(
        decoder->pcm_all + input_offset * decoder->audio.channels,
        info.data,
        info.bytes);
    decoder->pcm_all_samples = required_values;
    decoder->pcm_origin_sample = new_origin;
    return H2_PAL_OK;
}

static h2_pal_result_t drain_audio(
    h2_mp4_decoder_t *decoder,
    const h2_pal_audio_decoder_api_t *api,
    h2_pal_audio_decoder_session_t *session,
    int require_eos,
    size_t *out_frames) {
    *out_frames = 0u;
    for (;;) {
        h2_pal_audio_decoder_frame_t *frame = NULL;
        const h2_pal_result_t acquire =
            h2_pal_audio_decoder_acquire_frame(api, session, 0u, &frame);
        if (acquire == H2_PAL_EXIT) {
            return H2_PAL_EXIT;
        }
        if (acquire == H2_PAL_ERR_WOULD_BLOCK) {
            return require_eos ? H2_PAL_ERR_FORMAT : H2_PAL_OK;
        }
        if (acquire != H2_PAL_OK) {
            return acquire;
        }
        h2_pal_result_t result =
            append_audio_frame(decoder, api, session, frame);
        const h2_pal_result_t release =
            h2_pal_audio_decoder_release_frame(api, session, frame);
        if (result == H2_PAL_OK) {
            result = release;
        }
        if (result != H2_PAL_OK) {
            return result;
        }
        ++*out_frames;
    }
}

static h2_pal_result_t decode_audio(
    h2_mp4_decoder_t *decoder,
    h2_pal_audio_decoder_api_t api,
    size_t max_pcm_bytes) {
    decoder->pcm_all_limit = max_pcm_bytes / sizeof(int16_t);
    if (decoder->pcm_all_limit == 0u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t capacity = 0u;
    size_t capacity_bytes = 0u;
    if (!checked_mul_size(1024u, decoder->audio.channels, &capacity)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (capacity > decoder->pcm_all_limit) {
        capacity = decoder->pcm_all_limit;
    }
    if (!checked_mul_size(
            capacity, sizeof(int16_t), &capacity_bytes)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    decoder->pcm_all_capacity = capacity;
    decoder->pcm_all = h2_pal_mem_alloc(
        &decoder->allocator, capacity_bytes);
    if (decoder->pcm_all == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const h2_audio_decoder_config_t open_config = {
        .pcm_allocator = &decoder->allocator,
        .preferred_format = H2_AUDIO_SAMPLE_S16LE,
    };
    h2_pal_audio_decoder_session_t *session = NULL;
    h2_pal_result_t result = h2_pal_audio_decoder_open(&api, &open_config, &session);
    if (result != H2_PAL_OK) {
        return result;
    }
    const h2_audio_decoder_stream_config_t stream = {
        .codec = H2_AUDIO_CODEC_AAC_LC,
        .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
        .sample_rate_hz = decoder->audio.sample_rate,
        .channels = decoder->audio.channels,
        .codec_config = decoder->audio.codec_config,
        .codec_config_size = decoder->audio.codec_config_size,
    };
    result = h2_pal_audio_decoder_configure(&api, session, &stream);
    for (size_t i = 0u; result == H2_PAL_OK && i < decoder->audio.sample_count;) {
        const mp4_sample_t *sample = &decoder->audio.samples[i];
        const h2_audio_decoder_packet_t packet = {
            .data = decoder->file + sample->offset,
            .size = sample->size,
            .pts_us = sample->pts_us,
            .dts_us = sample->dts_us,
            .duration_us = sample->duration_us,
        };
        const h2_pal_result_t submit =
            h2_pal_audio_decoder_submit_packet(&api, session, &packet);
        if (submit == H2_PAL_OK) {
            ++i;
        } else if (submit != H2_PAL_ERR_WOULD_BLOCK) {
            result = submit;
            break;
        }
        size_t drained = 0u;
        result = drain_audio(decoder, &api, session, 0, &drained);
        if (result == H2_PAL_EXIT) {
            result = H2_PAL_ERR_FORMAT;
        } else if (result == H2_PAL_OK &&
                   submit == H2_PAL_ERR_WOULD_BLOCK && drained == 0u) {
            result = H2_PAL_ERR_WOULD_BLOCK;
        }
    }
    while (result == H2_PAL_OK) {
        const h2_audio_decoder_packet_t eos = {
            .flags = H2_AUDIO_DECODER_PACKET_END_OF_STREAM,
        };
        const h2_pal_result_t submit =
            h2_pal_audio_decoder_submit_packet(&api, session, &eos);
        if (submit == H2_PAL_OK) {
            break;
        }
        if (submit != H2_PAL_ERR_WOULD_BLOCK) {
            result = submit;
            break;
        }
        size_t drained = 0u;
        result = drain_audio(decoder, &api, session, 0, &drained);
        if (result == H2_PAL_EXIT) {
            result = H2_PAL_ERR_FORMAT;
        } else if (result == H2_PAL_OK && drained == 0u) {
            result = H2_PAL_ERR_WOULD_BLOCK;
        }
    }
    if (result == H2_PAL_OK) {
        size_t drained = 0u;
        result = drain_audio(decoder, &api, session, 1, &drained);
        if (result == H2_PAL_EXIT) result = H2_PAL_OK;
    }
    const h2_pal_result_t close = h2_pal_audio_decoder_close(&api, session);
    return result == H2_PAL_OK ? close : result;
}

static void cleanup(h2_mp4_decoder_t *decoder) {
    if (decoder == NULL) {
        return;
    }
    if (decoder->video_session != NULL) {
        (void)h2_pal_video_decoder_close(&decoder->video_api, decoder->video_session);
    }
    h2_pal_mem_free(&decoder->allocator, decoder->video.samples);
    h2_pal_mem_free(&decoder->allocator, decoder->audio.samples);
    h2_pal_mem_free(&decoder->allocator, decoder->video_codec_config);
    h2_pal_mem_free(&decoder->allocator, decoder->packet);
    h2_pal_mem_free(&decoder->allocator, decoder->video_storage);
    h2_pal_mem_free(&decoder->allocator, decoder->pcm_all);
    h2_pal_mem_free(&decoder->allocator, decoder->pcm_frame);
    h2_pal_mem_free(&decoder->allocator, decoder->file);
}

h2_pal_result_t h2_mp4_decoder_open(
    const h2_mp4_decoder_config_t *config,
    h2_mp4_decoder_t **out_decoder) {
    if (out_decoder == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_decoder = NULL;
    if (config == NULL || !h2_video_decoder_allocator_is_valid(config->allocator) ||
        config->source.read_at == NULL || config->source.size == 0u ||
        (config->video_decoder.vtable == NULL &&
         config->audio_decoder.vtable == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t max_file = config->max_file_bytes != 0u
        ? config->max_file_bytes : H2_MP4_DECODER_DEFAULT_MAX_FILE_BYTES;
    const size_t max_samples = config->max_samples != 0u
        ? config->max_samples : H2_MP4_DECODER_DEFAULT_MAX_SAMPLES;
    const size_t max_pcm_bytes = config->max_pcm_bytes != 0u
        ? config->max_pcm_bytes : H2_MP4_DECODER_DEFAULT_MAX_PCM_BYTES;
    const size_t max_presentation_bytes =
        config->max_presentation_bytes != 0u
        ? config->max_presentation_bytes
        : H2_MP4_DECODER_DEFAULT_MAX_PRESENTATION_BYTES;
    if (config->source.size > max_file || config->source.size > SIZE_MAX) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_mp4_decoder_t *decoder =
        h2_pal_mem_alloc(config->allocator, sizeof(*decoder));
    if (decoder == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->allocator = *config->allocator;
    decoder->video_api = config->video_decoder;
    decoder->video_format = config->video_format == H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED
        ? H2_VIDEO_PIXEL_FORMAT_RGB565 : config->video_format;
    decoder->frame.owner = decoder;
    decoder->file_size = (size_t)config->source.size;
    decoder->file = h2_pal_mem_alloc(&decoder->allocator, decoder->file_size);
    if (decoder->file == NULL) {
        h2_pal_mem_free(&decoder->allocator, decoder);
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t offset = 0u;
    while (offset < decoder->file_size) {
        size_t got = 0u;
        const h2_pal_result_t read_result = config->source.read_at(
            config->source.user, offset, decoder->file + offset,
            decoder->file_size - offset, &got);
        if (read_result != H2_PAL_OK || got == 0u || got > decoder->file_size - offset) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return read_result == H2_PAL_OK ? H2_PAL_ERR_IO : read_result;
        }
        offset += got;
    }
    mp4_box_t moov;
    if (!find_child(decoder->file, 0u, decoder->file_size, BOX('m','o','o','v'), &moov)) {
        cleanup(decoder);
        h2_pal_mem_free(&decoder->allocator, decoder);
        return H2_PAL_ERR_FORMAT;
    }
    size_t at = moov.payload;
    while (at < moov.end) {
        mp4_box_t child;
        if (!parse_box(decoder->file, moov.end, at, &child)) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_FORMAT;
        }
        if (child.type == BOX('t','r','a','k')) {
            mp4_track_t track = {0};
            if (parse_track(decoder, &child, &track, max_samples)) {
                if (track.kind == BOX('v','i','d','e') && decoder->video.samples == NULL) {
                    decoder->video = track;
                } else if (track.kind == BOX('s','o','u','n') && decoder->audio.samples == NULL) {
                    decoder->audio = track;
                } else {
                    h2_pal_mem_free(&decoder->allocator, track.samples);
                }
            }
        }
        at = child.end;
    }
    if ((config->require_video && decoder->video.samples == NULL) ||
        (config->require_audio && decoder->audio.samples == NULL) ||
        (decoder->video.samples == NULL && decoder->audio.samples == NULL)) {
        cleanup(decoder);
        h2_pal_mem_free(&decoder->allocator, decoder);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (decoder->video.samples != NULL && decoder->video_api.vtable != NULL) {
        if (!build_annex_b_config(decoder)) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_FORMAT;
        }
        size_t max_packet = 0u;
        for (size_t i = 0u; i < decoder->video.sample_count; ++i) {
            if (decoder->video.samples[i].size > max_packet) {
                max_packet = decoder->video.samples[i].size;
            }
        }
        if (!h2_mp4_decoder_annex_b_capacity(
                decoder->video.nal_length_size,
                max_packet,
                &decoder->packet_capacity)) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        decoder->packet = h2_pal_mem_alloc(&decoder->allocator, decoder->packet_capacity);
        if (!h2_mp4_decoder_video_frame_capacity(
                decoder->video_format,
                decoder->video.width,
                decoder->video.height,
                &decoder->video_storage_capacity) ||
            decoder->video_storage_capacity > max_presentation_bytes) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        decoder->video_storage_limit = max_presentation_bytes;
        decoder->video_storage =
            h2_pal_mem_alloc(&decoder->allocator, decoder->video_storage_capacity);
        if (decoder->packet == NULL || decoder->video_storage == NULL) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        const h2_pal_result_t result = configure_video(decoder);
        if (result != H2_PAL_OK) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return result;
        }
        decoder->info.has_video = 1;
        decoder->info.width = decoder->video.width;
        decoder->info.height = decoder->video.height;
        decoder->info.duration_us = decoder->video.duration_us;
    } else if (config->require_video && decoder->video.samples != NULL) {
        cleanup(decoder);
        h2_pal_mem_free(&decoder->allocator, decoder);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (decoder->audio.samples != NULL && config->audio_decoder.vtable != NULL) {
        const h2_pal_result_t result =
            decode_audio(decoder, config->audio_decoder, max_pcm_bytes);
        if (result == H2_PAL_OK) {
            decoder->info.has_audio = 1;
            decoder->info.audio_sample_rate_hz = decoder->audio.sample_rate;
            decoder->info.audio_channels = decoder->audio.channels;
            if (!decoder->info.has_video) {
                int64_t timeline_end = 0;
                if (!pcm_timeline_end(decoder, &timeline_end)) {
                    cleanup(decoder);
                    h2_pal_mem_free(&decoder->allocator, decoder);
                    return H2_PAL_ERR_FORMAT;
                }
                decoder->info.duration_us = timeline_end > 0
                    ? scale_us(timeline_end, decoder->audio.sample_rate)
                    : 0;
            }
        } else if (config->require_audio) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return result;
        } else {
            h2_pal_mem_free(&decoder->allocator, decoder->pcm_all);
            decoder->pcm_all = NULL;
            decoder->pcm_all_capacity = 0u;
            decoder->pcm_all_samples = 0u;
            decoder->pcm_all_limit = 0u;
            decoder->pcm_origin_sample = 0;
            decoder->pcm_has_origin = 0;
        }
    } else if (config->require_audio && decoder->audio.samples != NULL) {
        cleanup(decoder);
        h2_pal_mem_free(&decoder->allocator, decoder);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (decoder->info.has_audio && decoder->info.has_video &&
        decoder->video.samples != NULL) {
        int64_t max_duration = 0;
        for (size_t i = 0u; i < decoder->video.sample_count; ++i) {
            if (decoder->video.samples[i].duration_us > max_duration) {
                max_duration = decoder->video.samples[i].duration_us;
            }
        }
        size_t per_channel = 0u;
        size_t frame_bytes = 0u;
        if (!ceil_duration_samples(
                max_duration, decoder->audio.sample_rate, &per_channel) ||
            per_channel > SIZE_MAX - 2u) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        per_channel += 2u;
        if (!checked_mul_size(
                per_channel, decoder->audio.channels,
                &decoder->pcm_frame_capacity) ||
            !checked_mul_size(
                decoder->pcm_frame_capacity, sizeof(int16_t), &frame_bytes) ||
            frame_bytes > max_presentation_bytes) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        decoder->pcm_frame = h2_pal_mem_alloc(
            &decoder->allocator, frame_bytes);
        if (decoder->pcm_frame == NULL) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
    } else if (decoder->info.has_audio) {
        const size_t available_per_channel =
            decoder->pcm_all_samples / decoder->audio.channels;
        const size_t per_channel =
            available_per_channel < 1024u ? available_per_channel : 1024u;
        size_t frame_bytes = 0u;
        if (per_channel == 0u ||
            !checked_mul_size(
                per_channel, decoder->audio.channels,
                &decoder->pcm_frame_capacity) ||
            !checked_mul_size(
                decoder->pcm_frame_capacity, sizeof(int16_t), &frame_bytes) ||
            frame_bytes > max_presentation_bytes) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
        decoder->pcm_frame = h2_pal_mem_alloc(&decoder->allocator, frame_bytes);
        if (decoder->pcm_frame == NULL) {
            cleanup(decoder);
            h2_pal_mem_free(&decoder->allocator, decoder);
            return H2_PAL_ERR_NO_MEMORY;
        }
    }
    *out_decoder = decoder;
    return H2_PAL_OK;
}

h2_pal_result_t h2_mp4_decoder_get_info(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_info_t *out_info) {
    if (decoder == NULL || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_info = decoder->info;
    return H2_PAL_OK;
}

static h2_pal_result_t convert_sample(h2_mp4_decoder_t *decoder, size_t index, size_t *out_size) {
    const mp4_sample_t *sample = &decoder->video.samples[index];
    const uint8_t *input = decoder->file + sample->offset;
    size_t in = 0u;
    size_t out = 0u;
    while (in < sample->size) {
        if (decoder->video.nal_length_size > sample->size - in) {
            return H2_PAL_ERR_FORMAT;
        }
        uint32_t length = 0u;
        for (uint8_t i = 0u; i < decoder->video.nal_length_size; ++i) {
            length = (length << 8u) | input[in++];
        }
        if (length == 0u || length > sample->size - in ||
            out > decoder->packet_capacity - length - 4u) {
            return H2_PAL_ERR_FORMAT;
        }
        decoder->packet[out++] = 0u;
        decoder->packet[out++] = 0u;
        decoder->packet[out++] = 0u;
        decoder->packet[out++] = 1u;
        memcpy(decoder->packet + out, input + in, length);
        out += length;
        in += length;
    }
    *out_size = out;
    return H2_PAL_OK;
}

static h2_pal_result_t copy_pcm_interval(
    h2_mp4_decoder_t *decoder,
    int64_t start,
    int64_t end,
    size_t *out_samples_per_channel) {
    if (end < start) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t count = 0u;
    if (!checked_i64_span_size(start, end, &count)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t channels = decoder->audio.channels;
    size_t values = 0u;
    if (!checked_mul_size(count, channels, &values) ||
        values > decoder->pcm_frame_capacity) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (values != 0u) {
        memset(decoder->pcm_frame, 0, values * sizeof(int16_t));
    }
    if (count != 0u && decoder->pcm_has_origin) {
        int64_t timeline_end = 0;
        if (!pcm_timeline_end(decoder, &timeline_end)) {
            return H2_PAL_ERR_FORMAT;
        }
        const int64_t overlap_start =
            start > decoder->pcm_origin_sample
            ? start
            : decoder->pcm_origin_sample;
        const int64_t overlap_end = end < timeline_end ? end : timeline_end;
        if (overlap_end > overlap_start) {
            const size_t source =
                (size_t)(overlap_start - decoder->pcm_origin_sample);
            const size_t destination =
                (size_t)(overlap_start - start);
            const size_t overlap =
                (size_t)(overlap_end - overlap_start);
            memcpy(
                decoder->pcm_frame + destination * channels,
                decoder->pcm_all + source * channels,
                overlap * channels * sizeof(int16_t));
        }
    }
    *out_samples_per_channel = count;
    return H2_PAL_OK;
}

static h2_pal_result_t expose_video(
    h2_mp4_decoder_t *decoder,
    h2_pal_video_decoder_frame_t *decoded) {
    h2_video_frame_info_t info = {0};
    h2_pal_result_t result = h2_pal_video_decoder_frame_get_info(
        &decoder->video_api, decoder->video_session, decoded, &info);
    if (result != H2_PAL_OK || info.plane_count == 0u ||
        info.plane_count > H2_VIDEO_DECODER_MAX_PLANES) {
        return result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result;
    }
    size_t required = 0u;
    for (uint8_t i = 0u; i < info.plane_count; ++i) {
        if (info.planes[i].data == NULL ||
            info.planes[i].bytes > SIZE_MAX - required) {
            return H2_PAL_ERR_FORMAT;
        }
        required += info.planes[i].bytes;
    }
    result = reserve_video_storage(decoder, required);
    if (result != H2_PAL_OK) {
        return result;
    }
    size_t used = 0u;
    memset(&decoder->frame_info, 0, sizeof(decoder->frame_info));
    for (uint8_t i = 0u; i < info.plane_count; ++i) {
        memcpy(decoder->video_storage + used, info.planes[i].data, info.planes[i].bytes);
        decoder->frame_info.video_planes[i].data = decoder->video_storage + used;
        decoder->frame_info.video_planes[i].bytes = info.planes[i].bytes;
        decoder->frame_info.video_planes[i].stride_bytes = info.planes[i].stride_bytes;
        used += info.planes[i].bytes;
    }
    decoder->frame_info.pts_us = info.pts_us;
    decoder->frame_info.duration_us = info.duration_us;
    decoder->frame_info.video_format = info.format;
    decoder->frame_info.width = info.width;
    decoder->frame_info.height = info.height;
    decoder->frame_info.video_plane_count = info.plane_count;
    if (decoder->info.has_audio && info.duration_us >= 0) {
        if (info.pts_us > INT64_MAX - info.duration_us) {
            return H2_PAL_ERR_FORMAT;
        }
        int64_t start = 0;
        int64_t end = 0;
        if (!timestamp_to_signed_sample(
                info.pts_us, decoder->audio.sample_rate, &start) ||
            !timestamp_to_signed_sample(
                info.pts_us + info.duration_us,
                decoder->audio.sample_rate,
                &end)) {
            return H2_PAL_ERR_FORMAT;
        }
        size_t count = 0u;
        result = copy_pcm_interval(decoder, start, end, &count);
        if (result != H2_PAL_OK) {
            return result;
        }
        decoder->frame_info.pcm = decoder->pcm_frame;
        decoder->frame_info.pcm_samples_per_channel = count;
        decoder->frame_info.pcm_sample_rate_hz = decoder->audio.sample_rate;
        decoder->frame_info.pcm_channels = decoder->audio.channels;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t expose_audio_only(h2_mp4_decoder_t *decoder) {
    int64_t timeline_end = 0;
    if (!pcm_timeline_end(decoder, &timeline_end)) {
        return H2_PAL_ERR_FORMAT;
    }
    if (decoder->audio_cursor_sample >= timeline_end) {
        return H2_PAL_EXIT;
    }
    const size_t frame_capacity_per_channel =
        decoder->pcm_frame_capacity / decoder->audio.channels;
    if (frame_capacity_per_channel == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
#if SIZE_MAX > INT64_MAX
    if (frame_capacity_per_channel > (size_t)INT64_MAX) {
        return H2_PAL_ERR_FORMAT;
    }
#endif
    if (decoder->audio_cursor_sample >
            INT64_MAX - (int64_t)frame_capacity_per_channel) {
        return H2_PAL_ERR_FORMAT;
    }
    int64_t end =
        decoder->audio_cursor_sample +
        (int64_t)frame_capacity_per_channel;
    if (end > timeline_end) {
        end = timeline_end;
    }
    size_t count = 0u;
    h2_pal_result_t result = copy_pcm_interval(
        decoder, decoder->audio_cursor_sample, end, &count);
    if (result != H2_PAL_OK || count == 0u) {
        return result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result;
    }
    memset(&decoder->frame_info, 0, sizeof(decoder->frame_info));
    decoder->frame_info.pts_us = scale_us(
        decoder->audio_cursor_sample,
        decoder->audio.sample_rate);
    decoder->frame_info.duration_us =
        scale_us((int64_t)count, decoder->audio.sample_rate);
    if (decoder->frame_info.pts_us == INT64_MIN ||
        decoder->frame_info.duration_us == INT64_MIN) {
        return H2_PAL_ERR_FORMAT;
    }
    decoder->frame_info.pcm = decoder->pcm_frame;
    decoder->frame_info.pcm_samples_per_channel = count;
    decoder->frame_info.pcm_sample_rate_hz = decoder->audio.sample_rate;
    decoder->frame_info.pcm_channels = decoder->audio.channels;
    decoder->audio_cursor_sample = end;
    return H2_PAL_OK;
}

h2_pal_result_t h2_mp4_decoder_acquire_frame(
    h2_mp4_decoder_t *decoder,
    uint32_t timeout_ms,
    h2_mp4_decoder_frame_t **out_frame) {
    if (out_frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_frame = NULL;
    if (decoder == NULL ||
        (decoder->video_session == NULL && !decoder->info.has_audio)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (decoder->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (decoder->video_session == NULL) {
        const h2_pal_result_t result = expose_audio_only(decoder);
        if (result != H2_PAL_OK) {
            return result;
        }
        decoder->acquired = 1;
        *out_frame = &decoder->frame;
        return H2_PAL_OK;
    }
    uint32_t backend_timeout_ms = 0u;
    int timed_wait_used = timeout_ms == 0u;
    for (;;) {
        h2_pal_video_decoder_frame_t *decoded = NULL;
        h2_pal_result_t result = h2_pal_video_decoder_acquire_frame(
            &decoder->video_api,
            decoder->video_session,
            backend_timeout_ms,
            &decoded);
        backend_timeout_ms = 0u;
        if (result == H2_PAL_OK) {
            result = expose_video(decoder, decoded);
            const h2_pal_result_t release = h2_pal_video_decoder_release_frame(
                &decoder->video_api, decoder->video_session, decoded);
            if (result == H2_PAL_OK) {
                result = release;
            }
            if (result != H2_PAL_OK) {
                return result;
            }
            decoder->acquired = 1;
            *out_frame = &decoder->frame;
            return H2_PAL_OK;
        }
        if (result == H2_PAL_EXIT) {
            return H2_PAL_EXIT;
        }
        if (result == H2_PAL_ERR_TIMEOUT) {
            return H2_PAL_ERR_TIMEOUT;
        }
        if (result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        if (decoder->video_sample_index < decoder->video.sample_count) {
            size_t packet_size = 0u;
            result = convert_sample(decoder, decoder->video_sample_index, &packet_size);
            if (result != H2_PAL_OK) {
                return result;
            }
            const mp4_sample_t *sample =
                &decoder->video.samples[decoder->video_sample_index];
            const h2_video_decoder_packet_t packet = {
                .data = decoder->packet,
                .size = packet_size,
                .pts_us = sample->pts_us,
                .dts_us = sample->dts_us,
                .duration_us = sample->duration_us,
            };
            result = h2_pal_video_decoder_submit_packet(
                &decoder->video_api, decoder->video_session, &packet);
            if (result == H2_PAL_OK) {
                ++decoder->video_sample_index;
                continue;
            }
            if (result != H2_PAL_ERR_WOULD_BLOCK) {
                return result;
            }
        } else if (!decoder->video_eos_submitted) {
            const h2_video_decoder_packet_t eos = {
                .flags = H2_VIDEO_DECODER_PACKET_END_OF_STREAM,
            };
            result = h2_pal_video_decoder_submit_packet(
                &decoder->video_api, decoder->video_session, &eos);
            if (result == H2_PAL_OK) {
                decoder->video_eos_submitted = 1;
                continue;
            }
            if (result != H2_PAL_ERR_WOULD_BLOCK) {
                return result;
            }
        } else {
            if (timed_wait_used) {
                return H2_PAL_ERR_WOULD_BLOCK;
            }
            timed_wait_used = 1;
            backend_timeout_ms = timeout_ms;
            continue;
        }
        if (timed_wait_used) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        timed_wait_used = 1;
        backend_timeout_ms = timeout_ms;
    }
}

h2_pal_result_t h2_mp4_decoder_frame_get_info(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_frame_t *frame,
    h2_mp4_decoder_frame_info_t *out_info) {
    if (decoder == NULL || frame == NULL || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (!decoder->acquired || frame != &decoder->frame || frame->owner != decoder) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = decoder->frame_info;
    return H2_PAL_OK;
}

h2_pal_result_t h2_mp4_decoder_release_frame(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_frame_t *frame) {
    if (decoder == NULL || frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!decoder->acquired || frame != &decoder->frame || frame->owner != decoder) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    decoder->acquired = 0;
    memset(&decoder->frame_info, 0, sizeof(decoder->frame_info));
    return H2_PAL_OK;
}

h2_pal_result_t h2_mp4_decoder_seek(h2_mp4_decoder_t *decoder, int64_t target_us) {
    if (decoder == NULL || target_us < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (decoder->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (decoder->video_session == NULL) {
        if (!decoder->info.has_audio) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        int64_t timeline_end = 0;
        int64_t target_sample = 0;
        if (!pcm_timeline_end(decoder, &timeline_end) ||
            !timestamp_to_signed_sample(
                target_us, decoder->audio.sample_rate, &target_sample)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (target_sample < 0) {
            target_sample = 0;
        }
        decoder->audio_cursor_sample =
            target_sample > timeline_end ? timeline_end : target_sample;
        return H2_PAL_OK;
    }
    size_t index = 0u;
    for (size_t i = 0u; i < decoder->video.sample_count; ++i) {
        if (decoder->video.samples[i].pts_us > target_us) {
            break;
        }
        if (decoder->video.samples[i].sync) {
            index = i;
        }
    }
    h2_pal_result_t result =
        h2_pal_video_decoder_reset(&decoder->video_api, decoder->video_session);
    if (result != H2_PAL_OK) {
        return result;
    }
    const h2_video_decoder_stream_config_t stream = {
        .codec = H2_VIDEO_CODEC_H264,
        .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
        .coded_width = decoder->video.width,
        .coded_height = decoder->video.height,
        .visible_width = decoder->video.width,
        .visible_height = decoder->video.height,
        .codec_config = decoder->video_codec_config,
        .codec_config_size = decoder->video_codec_config_size,
    };
    result = h2_pal_video_decoder_configure(
        &decoder->video_api, decoder->video_session, &stream);
    if (result == H2_PAL_OK) {
        decoder->video_sample_index = index;
        decoder->video_eos_submitted = 0;
    }
    return result;
}

h2_pal_result_t h2_mp4_decoder_reset(h2_mp4_decoder_t *decoder) {
    return h2_mp4_decoder_seek(decoder, 0);
}

h2_pal_result_t h2_mp4_decoder_close(h2_mp4_decoder_t *decoder) {
    if (decoder == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (decoder->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = decoder->allocator;
    cleanup(decoder);
    h2_pal_mem_free(&allocator, decoder);
    return H2_PAL_OK;
}
