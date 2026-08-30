#include "h2_loader_boot.h"
#include "h2_loader_status.h"

#include <string.h>

#define H2_LOADER_MFG_KEY "mfg"
#define H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY "mfg_acceptance_revision"
#define H2_LOADER_MFG_RECORD_V1_FORMAT 1u
#define H2_LOADER_MFG_RECORD_V1_SIZE 16u
#define H2_LOADER_MFG_RECORD_V2_FORMAT 2u
#define H2_LOADER_MFG_RECORD_V2_SIZE 24u
#define H2_LOADER_MFG_RECORD_FORMAT 3u
#define H2_LOADER_MFG_RECORD_SIZE (4u + H2_LOADER_MFG_STEP_TOTAL)

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

static int mfg_record_decode(
    const void *data,
    size_t len,
    h2_loader_mfg_summary_t *out_summary) {
    const uint8_t *cursor = (const uint8_t *)data;
    uint32_t format;
    uint32_t state;
    uint32_t passed;
    uint32_t total;
    uint32_t passed_mask = 0u;
    uint32_t skipped_mask = 0u;

    if (data == NULL || out_summary == NULL ||
        (len != H2_LOADER_MFG_RECORD_V1_SIZE &&
         len != H2_LOADER_MFG_RECORD_V2_SIZE &&
         len != H2_LOADER_MFG_RECORD_SIZE)) {
        return H2_PAL_ERR_FORMAT;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    format = get_u32_le(&cursor);
    if (format == H2_LOADER_MFG_RECORD_FORMAT &&
        len == H2_LOADER_MFG_RECORD_SIZE) {
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        memcpy(out_summary->step_status, cursor,
               sizeof(out_summary->step_status));
    } else if ((format == H2_LOADER_MFG_RECORD_V1_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V1_SIZE) ||
               (format == H2_LOADER_MFG_RECORD_V2_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V2_SIZE)) {
        state = get_u32_le(&cursor);
        passed = get_u32_le(&cursor);
        total = get_u32_le(&cursor);
        if (total != H2_LOADER_MFG_STEP_TOTAL || passed > total ||
            state < 1u || state > 3u) {
            return H2_PAL_ERR_FORMAT;
        }
        if (format == H2_LOADER_MFG_RECORD_V2_FORMAT) {
            const uint32_t valid_mask =
                (UINT32_C(1) << H2_LOADER_MFG_STEP_TOTAL) - UINT32_C(1);
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
            (passed != H2_LOADER_MFG_STEP_TOTAL ||
             passed_mask !=
                 (UINT32_C(1) << H2_LOADER_MFG_STEP_TOTAL) - UINT32_C(1) ||
             skipped_mask != 0u)) {
            return H2_PAL_ERR_FORMAT;
        }
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
            if ((passed_mask & (UINT32_C(1) << i)) != 0u) {
                out_summary->step_status[i] = H2_LOADER_MFG_STEP_PASSED;
            } else if ((skipped_mask & (UINT32_C(1) << i)) != 0u) {
                out_summary->step_status[i] = H2_LOADER_MFG_STEP_SKIPPED;
            }
        }
        if (state == 3u && passed < H2_LOADER_MFG_STEP_TOTAL &&
            out_summary->step_status[passed] == H2_LOADER_MFG_STEP_UNTESTED) {
            out_summary->step_status[passed] = H2_LOADER_MFG_STEP_FAILED;
        }
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    return h2_loader_mfg_summary_validate(out_summary) == H2_PAL_OK
        ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

int h2_loader_mfg_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present) {
    h2_pal_pref_namespace_t *ns = NULL;
    void *data = NULL;
    size_t len = 0u;
    int rc;
    int close_rc;

    if (pref == NULL || allocator == NULL || out_summary == NULL ||
        out_present == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    *out_present = 0;
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
    rc = mfg_record_decode(data, len, out_summary);
    const int needs_migration =
        rc == H2_PAL_OK && len != H2_LOADER_MFG_RECORD_SIZE;
    h2_pal_mem_free(allocator, data);
    if (rc == H2_PAL_ERR_FORMAT) {
        memset(out_summary, 0, sizeof(*out_summary));
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        rc = h2_loader_mfg_write(pref, out_summary);
    } else if (needs_migration) {
        rc = h2_loader_mfg_write(pref, out_summary);
    }
    return rc;
}

int h2_loader_mfg_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_mfg_summary_t *summary) {
    uint8_t data[H2_LOADER_MFG_RECORD_SIZE];
    uint8_t *cursor = data;
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (h2_loader_mfg_summary_validate(summary) != H2_PAL_OK ||
        summary->total != H2_LOADER_MFG_STEP_TOTAL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    put_u32_le(&cursor, H2_LOADER_MFG_RECORD_FORMAT);
    memcpy(cursor, summary->step_status, sizeof(summary->step_status));
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->set_blob == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, H2_LOADER_MFG_KEY, data, sizeof(data));
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

int h2_loader_mfg_ensure_acceptance_revision(
    const h2_pal_pref_api_t *pref,
    uint32_t total,
    uint32_t required_revision) {
    uint32_t stored_revision = 0u;
    int rc;
    if (pref == NULL || total == 0u || required_revision == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_get_u32(
        pref, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY, &stored_revision);
    if (rc == H2_PAL_OK && stored_revision == required_revision) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) return rc;
    rc = h2_loader_mfg_reset(pref, total);
    return rc == H2_PAL_OK
        ? pref_set_u32(pref, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY,
              required_revision)
        : rc;
}
