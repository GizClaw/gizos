#include "h2_loader_boot.h"
#include "h2_loader_status.h"

#include <string.h>

#define H2_LOADER_MFG_KEY "mfg"
#define H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY "mfg_acceptance_revision"

/*
 * Persisted MFG record layouts (pref namespace "h2loader", blob key "mfg").
 * All integers are little-endian.
 *
 * v1 (16 bytes):  u32 format=1, u32 state, u32 passed, u32 total=22.
 * v2 (24 bytes):  v1 fields with format=2, then u32 passed_mask,
 *                 u32 skipped_mask.
 * v3 (26 bytes):  u32 format=3, u8 step_status[22].
 * v4 (5 + total): u32 format=4, u8 total (1..H2_LOADER_MFG_STEP_MAX),
 *                 u8 step_status[total]. The blob length must be exactly
 *                 5 + total; each byte is an h2_loader_mfg_step_status_t.
 *
 * v1-v3 always carry H2_LOADER_MFG_LEGACY_STEP_TOTAL steps. Only v4 is
 * written; a legacy record is rewritten as v4 the first time it is read.
 */
#define H2_LOADER_MFG_RECORD_V1_FORMAT 1u
#define H2_LOADER_MFG_RECORD_V1_SIZE 16u
#define H2_LOADER_MFG_RECORD_V2_FORMAT 2u
#define H2_LOADER_MFG_RECORD_V2_SIZE 24u
#define H2_LOADER_MFG_RECORD_V3_FORMAT 3u
#define H2_LOADER_MFG_RECORD_V3_SIZE (4u + H2_LOADER_MFG_LEGACY_STEP_TOTAL)
#define H2_LOADER_MFG_RECORD_FORMAT 4u
#define H2_LOADER_MFG_RECORD_HEADER_SIZE 5u
#define H2_LOADER_MFG_RECORD_MAX_SIZE \
    (H2_LOADER_MFG_RECORD_HEADER_SIZE + H2_LOADER_MFG_STEP_MAX)

_Static_assert(H2_LOADER_MFG_STEP_MAX <= 255u,
               "v4 MFG record stores total in one byte");
_Static_assert(H2_LOADER_MFG_LEGACY_STEP_TOTAL <= H2_LOADER_MFG_STEP_MAX &&
                   H2_LOADER_MFG_LEGACY_STEP_TOTAL < 32u,
               "legacy MFG masks are 32-bit");
_Static_assert(H2_LOADER_MFG_RECORD_MAX_SIZE >= H2_LOADER_MFG_RECORD_V3_SIZE &&
                   H2_LOADER_MFG_RECORD_MAX_SIZE >=
                       H2_LOADER_MFG_RECORD_V2_SIZE,
               "MFG record buffer must hold every accepted layout");

static void put_u32_le(uint8_t **cursor, uint32_t value) {
    for (size_t i = 0u; i < 4u; ++i) {
        (*cursor)[i] = (uint8_t)(value >> (i * 8u));
    }
    *cursor += 4u;
}

static uint32_t get_u32_le(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (size_t i = 0u; i < 4u; ++i) {
        value |= (uint32_t)(*cursor)[i] << (i * 8u);
    }
    *cursor += 4u;
    return value;
}

static int pref_open(
    const h2_pal_pref_api_t *pref,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_ns) {
    if (pref == NULL || out_ns == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_ns = NULL;
    return h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE, mode, out_ns);
}

static int mfg_record_decode_legacy_counters(
    uint32_t format,
    const uint8_t *cursor,
    h2_loader_mfg_summary_t *out_summary) {
    const uint32_t valid_mask =
        (UINT32_C(1) << H2_LOADER_MFG_LEGACY_STEP_TOTAL) - UINT32_C(1);
    const uint32_t state = get_u32_le(&cursor);
    const uint32_t passed = get_u32_le(&cursor);
    const uint32_t total = get_u32_le(&cursor);
    uint32_t passed_mask = 0u;
    uint32_t skipped_mask = 0u;

    if (total != H2_LOADER_MFG_LEGACY_STEP_TOTAL || passed > total ||
        state < 1u || state > 3u) {
        return H2_PAL_ERR_FORMAT;
    }
    if (format == H2_LOADER_MFG_RECORD_V2_FORMAT) {
        passed_mask = get_u32_le(&cursor);
        skipped_mask = get_u32_le(&cursor);
        if ((passed_mask & skipped_mask) != 0u ||
            ((passed_mask | skipped_mask) & ~valid_mask) != 0u) {
            return H2_PAL_ERR_FORMAT;
        }
    } else if (passed > 0u) {
        passed_mask = (UINT32_C(1) << passed) - UINT32_C(1);
    }
    if (state == 2u &&
        (passed != H2_LOADER_MFG_LEGACY_STEP_TOTAL ||
         passed_mask != valid_mask || skipped_mask != 0u)) {
        return H2_PAL_ERR_FORMAT;
    }
    out_summary->total = H2_LOADER_MFG_LEGACY_STEP_TOTAL;
    for (uint32_t i = 0u; i < H2_LOADER_MFG_LEGACY_STEP_TOTAL; ++i) {
        if ((passed_mask & (UINT32_C(1) << i)) != 0u) {
            out_summary->step_status[i] = H2_LOADER_MFG_STEP_PASSED;
        } else if ((skipped_mask & (UINT32_C(1) << i)) != 0u) {
            out_summary->step_status[i] = H2_LOADER_MFG_STEP_SKIPPED;
        }
    }
    if (state == 3u && passed < H2_LOADER_MFG_LEGACY_STEP_TOTAL &&
        out_summary->step_status[passed] == H2_LOADER_MFG_STEP_UNTESTED) {
        out_summary->step_status[passed] = H2_LOADER_MFG_STEP_FAILED;
    }
    return H2_PAL_OK;
}

static int mfg_record_decode(
    const void *data,
    size_t len,
    h2_loader_mfg_summary_t *out_summary,
    uint32_t *out_format) {
    const uint8_t *cursor = (const uint8_t *)data;
    uint32_t format;
    int rc = H2_PAL_ERR_FORMAT;

    if (data == NULL || out_summary == NULL || out_format == NULL ||
        len < 4u) {
        return H2_PAL_ERR_FORMAT;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    format = get_u32_le(&cursor);
    *out_format = format;
    if (format == H2_LOADER_MFG_RECORD_FORMAT) {
        const uint32_t total =
            len >= H2_LOADER_MFG_RECORD_HEADER_SIZE ? cursor[0] : 0u;
        if (total != 0u && total <= H2_LOADER_MFG_STEP_MAX &&
            len == H2_LOADER_MFG_RECORD_HEADER_SIZE + total) {
            out_summary->total = total;
            memcpy(out_summary->step_status, cursor + 1u, total);
            rc = H2_PAL_OK;
        }
    } else if (format == H2_LOADER_MFG_RECORD_V3_FORMAT &&
               len == H2_LOADER_MFG_RECORD_V3_SIZE) {
        out_summary->total = H2_LOADER_MFG_LEGACY_STEP_TOTAL;
        memcpy(out_summary->step_status, cursor,
               H2_LOADER_MFG_LEGACY_STEP_TOTAL);
        rc = H2_PAL_OK;
    } else if ((format == H2_LOADER_MFG_RECORD_V1_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V1_SIZE) ||
               (format == H2_LOADER_MFG_RECORD_V2_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V2_SIZE)) {
        rc = mfg_record_decode_legacy_counters(format, cursor, out_summary);
    }
    if (rc == H2_PAL_OK &&
        h2_loader_mfg_summary_validate(out_summary) != H2_PAL_OK) {
        rc = H2_PAL_ERR_FORMAT;
    }
    return rc;
}

/*
 * Loads and decodes the stored record without rewriting it. Returns H2_PAL_OK
 * with *out_present == 0 when there is no record, H2_PAL_ERR_FORMAT when a
 * record exists but cannot be decoded, or the pref error otherwise.
 */
static int mfg_record_load(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present,
    int *out_needs_migration) {
    h2_pal_pref_namespace_t *ns = NULL;
    void *data = NULL;
    size_t len = 0u;
    uint32_t format = 0u;
    int rc;
    int close_rc;

    memset(out_summary, 0, sizeof(*out_summary));
    *out_present = 0;
    *out_needs_migration = 0;
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    rc = ns != NULL && ns->get_blob != NULL
        ? ns->get_blob(ns, allocator, H2_LOADER_MFG_KEY, &data, &len)
        : H2_PAL_ERR_UNSUPPORTED;
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    if (rc == H2_PAL_ERR_NOT_FOUND) return close_rc;
    if (rc != H2_PAL_OK || close_rc != H2_PAL_OK) {
        h2_pal_mem_free(allocator, data);
        return rc != H2_PAL_OK ? rc : close_rc;
    }
    *out_present = 1;
    rc = mfg_record_decode(data, len, out_summary, &format);
    h2_pal_mem_free(allocator, data);
    if (rc != H2_PAL_OK) {
        memset(out_summary, 0, sizeof(*out_summary));
        return H2_PAL_ERR_FORMAT;
    }
    *out_needs_migration = format != H2_LOADER_MFG_RECORD_FORMAT;
    return H2_PAL_OK;
}

int h2_loader_mfg_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present) {
    int needs_migration = 0;
    int rc;

    if (pref == NULL || allocator == NULL || out_summary == NULL ||
        out_present == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = mfg_record_load(
        pref, allocator, out_summary, out_present, &needs_migration);
    if (rc == H2_PAL_ERR_FORMAT) {
        memset(out_summary, 0, sizeof(*out_summary));
        out_summary->total = H2_LOADER_MFG_LEGACY_STEP_TOTAL;
        rc = h2_loader_mfg_write(pref, out_summary);
    } else if (rc == H2_PAL_OK && needs_migration) {
        rc = h2_loader_mfg_write(pref, out_summary);
    }
    return rc;
}

int h2_loader_mfg_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_mfg_summary_t *summary) {
    uint8_t data[H2_LOADER_MFG_RECORD_MAX_SIZE];
    uint8_t *cursor = data;
    size_t len;
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (h2_loader_mfg_summary_validate(summary) != H2_PAL_OK ||
        summary->total == 0u || summary->total > H2_LOADER_MFG_STEP_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    put_u32_le(&cursor, H2_LOADER_MFG_RECORD_FORMAT);
    *cursor++ = (uint8_t)summary->total;
    memcpy(cursor, summary->step_status, summary->total);
    len = H2_LOADER_MFG_RECORD_HEADER_SIZE + summary->total;
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->set_blob == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, H2_LOADER_MFG_KEY, data, len);
        if (rc == H2_PAL_OK) rc = ns->commit(ns);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_mfg_reset(
    const h2_pal_pref_api_t *pref,
    uint32_t total) {
    const h2_loader_mfg_summary_t summary = {.total = total};
    return h2_loader_mfg_write(pref, &summary);
}

static int pref_get_u32(
    const h2_pal_pref_api_t *pref,
    const char *key,
    uint32_t *out_value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    int close_rc;
    if (rc != H2_PAL_OK) return rc;
    rc = ns != NULL && ns->get_u32 != NULL
        ? ns->get_u32(ns, key, out_value) : H2_PAL_ERR_UNSUPPORTED;
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_set_u32(
    const h2_pal_pref_api_t *pref,
    const char *key,
    uint32_t value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->set_u32 == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_u32(ns, key, value);
        if (rc == H2_PAL_OK) rc = ns->commit(ns);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

/*
 * Single-shot allocator backed by caller-owned storage, so the acceptance
 * check can read the record without an allocator argument. A request larger
 * than any accepted record layout fails and is remembered as oversize.
 */
typedef struct mfg_record_buffer {
    uint8_t data[H2_LOADER_MFG_RECORD_MAX_SIZE];
    int in_use;
    int oversize;
} mfg_record_buffer_t;

static void *mfg_record_buffer_alloc(void *user, size_t len) {
    mfg_record_buffer_t *buffer = (mfg_record_buffer_t *)user;
    if (buffer == NULL || buffer->in_use) return NULL;
    if (len > sizeof(buffer->data)) {
        buffer->oversize = 1;
        return NULL;
    }
    buffer->in_use = 1;
    return buffer->data;
}

static void *mfg_record_buffer_realloc(void *user, void *ptr, size_t len) {
    mfg_record_buffer_t *buffer = (mfg_record_buffer_t *)user;
    if (ptr == NULL) return mfg_record_buffer_alloc(user, len);
    if (buffer == NULL || ptr != buffer->data) return NULL;
    if (len > sizeof(buffer->data)) {
        buffer->oversize = 1;
        return NULL;
    }
    return ptr;
}

static void mfg_record_buffer_free(void *user, void *ptr) {
    mfg_record_buffer_t *buffer = (mfg_record_buffer_t *)user;
    if (buffer != NULL && ptr == buffer->data) buffer->in_use = 0;
}

static const h2_pal_mem_vtable_t mfg_record_buffer_vtable = {
    .alloc = mfg_record_buffer_alloc,
    .realloc = mfg_record_buffer_realloc,
    .free = mfg_record_buffer_free,
};

int h2_loader_mfg_ensure_acceptance_revision(
    const h2_pal_pref_api_t *pref,
    uint32_t total,
    uint32_t required_revision) {
    mfg_record_buffer_t buffer;
    const h2_pal_mem_api_t allocator = {
        .user = &buffer,
        .vtable = &mfg_record_buffer_vtable,
    };
    h2_loader_mfg_summary_t stored;
    uint32_t stored_revision = 0u;
    int present = 0;
    int needs_migration = 0;
    int rc;

    if (pref == NULL || total == 0u || total > H2_LOADER_MFG_STEP_MAX ||
        required_revision == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_get_u32(
        pref, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY, &stored_revision);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) return rc;
    if (rc == H2_PAL_OK && stored_revision == required_revision) {
        memset(&buffer, 0, sizeof(buffer));
        rc = mfg_record_load(
            pref, &allocator, &stored, &present, &needs_migration);
        if (rc == H2_PAL_ERR_NO_MEMORY && buffer.oversize) {
            rc = H2_PAL_ERR_FORMAT;
        }
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_FORMAT) return rc;
        if (rc == H2_PAL_OK && present && stored.total == total) {
            return H2_PAL_OK;
        }
    }
    rc = h2_loader_mfg_reset(pref, total);
    return rc == H2_PAL_OK
        ? pref_set_u32(pref, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY,
              required_revision)
        : rc;
}
