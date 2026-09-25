#include "h2_mem_arena_diagnostics.h"
#include "h2_mem_arena_census.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_DIAG_MAX_TAGS 64u
#define H2_DIAG_FRAMES 8u
#define H2_DIAG_PHASE 64u
#define H2_DIAG_SCAN_CHUNK 16384u
#define H2_DIAG_PATTERN 0xd3u
#define H2_DIAG_LARGE 16384u

typedef struct diag_block {
    void *ptr;
    uint64_t id;
    size_t requested;
    size_t consumed;
    size_t tag;
    size_t site;
    unsigned pool;
    bool fallback;
} diag_block_t;

typedef struct diag_tag {
    size_t live, peak, consumed, overhead_peak, allocations, failures, largest;
    size_t freed_requested, freed_unused, unused_max;
    size_t end_first, end_last, end_min, end_max;
} diag_tag_t;

typedef struct diag_site {
    uintptr_t frames[H2_DIAG_FRAMES];
    size_t live, peak, consumed, overhead_peak, requests, requested;
    size_t freed_requested, freed_unused, retired_requested, retired_unused;
    size_t realloc_count, growth_bytes, moved, unused_snapshot_max;
} diag_site_t;

typedef struct diag_peak {
    uint64_t seq;
    size_t live, consumed, fallback;
    size_t tags[H2_DIAG_MAX_TAGS];
    size_t consumed_tags[H2_DIAG_MAX_TAGS];
} diag_peak_t;

typedef struct diag_duplicate {
    uint64_t first, second, snapshot;
    size_t bytes;
} diag_duplicate_t;
typedef struct diag_hash_sample {
    size_t used;
    uint64_t hash;
    diag_block_t block;
} diag_hash_sample_t;

typedef struct diag_snapshot {
    char phase[H2_DIAG_PHASE];
    uint64_t seq, allocation_seq;
    size_t live, consumed, block_count, site_count, peak_count;
    size_t block_overflow, site_overflow, peak_overflow, report_backpressure;
    size_t case_ends;
    diag_block_t *blocks;
    diag_block_t *top_blocks;
    size_t top_count;
    diag_site_t *sites;
    diag_peak_t *peaks;
    diag_tag_t tags[H2_DIAG_MAX_TAGS];
    diag_peak_t peak;
    h2_mem_arena_pool_inspection_t pools[2];
    h2_mem_arena_stats_t arena_stats;
    uint64_t captured_us;
    uint64_t capture_elapsed_us;
    size_t census_live[H2_DIAG_MAX_TAGS];
    size_t census_peak[H2_DIAG_MAX_TAGS];
    size_t census_unattributed_live, census_block_overflow, census_site_overflow;
} diag_snapshot_t;

typedef struct diag_view {
    h2_pal_mem_api_t api;
    struct h2_mem_arena_diagnostics *owner;
    size_t tag;
} diag_view_t;

struct h2_mem_arena_diagnostics {
    h2_mem_arena_diagnostics_config_t config;
    h2_mem_arena_t *arena;
    h2_mem_arena_census_t *census;
    h2_mem_arena_census_tag_config_t *census_tags;
    diag_view_t *views;
    diag_block_t *blocks;
    diag_site_t *sites;
    diag_peak_t *peaks;
    diag_duplicate_t *duplicates;
    diag_hash_sample_t *hash_samples;
    diag_block_t *peak_blocks;
    size_t peak_block_count;
    uint64_t *resident_ids;
    size_t resident_count;
    unsigned char *tail_scratch, *report_scratch;
    size_t site_count, peak_count, duplicate_count;
    size_t block_overflow, site_overflow, peak_overflow, duplicate_overflow;
    size_t report_backpressure;
    size_t live, consumed, fallback_live, consumed_peak, overlap_upper;
    uint64_t sequence, next_id, snapshot_sequence, case_ends;
    diag_tag_t tags[H2_DIAG_MAX_TAGS];
    diag_peak_t peak;
    size_t polled_peak;
    uint64_t last_poll_ms;
    h2_pal_mutex_t *lock, *core_lock, *report_lock;
    h2_pal_semaphore_t *flush_done;
    h2_pal_queue_t *reports;
    h2_pal_task_t *reporter;
    h2_pal_fs_file_t *file;
    h2_pal_result_t report_result;
    size_t scan_unsupported;
    size_t *site_unused_scratch;
    size_t *site_rank_scratch;
};

static void diag_lock(h2_mem_arena_diagnostics_t *d, h2_pal_mutex_t *mutex) {
    if (h2_pal_mutex_lock(d->config.sync, mutex) != H2_PAL_OK)
        abort();
}
static void diag_unlock(h2_mem_arena_diagnostics_t *d, h2_pal_mutex_t *mutex) {
    if (h2_pal_mutex_unlock(d->config.sync, mutex) != H2_PAL_OK)
        abort();
}
static void arena_lock(void *user) {
    diag_lock(user, ((h2_mem_arena_diagnostics_t *)user)->core_lock);
}
static void arena_unlock(void *user) {
    diag_unlock(user, ((h2_mem_arena_diagnostics_t *)user)->core_lock);
}
static void *metadata_alloc(h2_mem_arena_diagnostics_t *d, size_t count,
                            size_t size) {
    if (size == 0u || count > SIZE_MAX / size)
        return NULL;
    void *ptr = h2_pal_mem_alloc(d->config.metadata, count * size);
    if (ptr != NULL)
        memset(ptr, 0, count * size);
    return ptr;
}
static const h2_pal_mem_api_t *business_mem(h2_mem_arena_diagnostics_t *d,
                                            size_t tag) {
    return d->census != NULL ? h2_mem_arena_census_tag_mem(d->census, tag)
                             : h2_mem_arena_mem(d->arena);
}
static h2_mem_arena_block_info_t block_info(h2_mem_arena_diagnostics_t *d,
                                             const void *ptr) {
    h2_mem_arena_block_info_t info = {0};
    if (h2_mem_arena_block_info(d->arena, ptr, &info) != H2_PAL_OK)
        abort();
    return info;
}
static size_t block_slot(h2_mem_arena_diagnostics_t *d, const void *ptr) {
    for (size_t i = 0u; i < d->config.block_capacity; ++i)
        if (d->blocks[i].ptr == ptr)
            return i;
    return SIZE_MAX;
}
static size_t empty_block_slot(h2_mem_arena_diagnostics_t *d) {
    for (size_t i = 0u; i < d->config.block_capacity; ++i)
        if (d->blocks[i].ptr == NULL)
            return i;
    return SIZE_MAX;
}
static size_t capture_site(h2_mem_arena_diagnostics_t *d,
                           uintptr_t frames[H2_DIAG_FRAMES]) {
    memset(frames, 0, H2_DIAG_FRAMES * sizeof(*frames));
    if (!d->config.trace || d->config.probe.capture_frames == NULL)
        return 0u;
    size_t count = 0u;
    if (d->config.probe.capture_frames(d->config.probe.user, frames,
                                       H2_DIAG_FRAMES, &count) != H2_PAL_OK)
        memset(frames, 0, H2_DIAG_FRAMES * sizeof(*frames));
    else if (count > H2_DIAG_FRAMES)
        count = H2_DIAG_FRAMES;
    return count;
}
static size_t site_slot(h2_mem_arena_diagnostics_t *d,
                        const uintptr_t frames[H2_DIAG_FRAMES]) {
    if (!d->config.trace || frames[0] == 0u)
        return 0u;
    for (size_t i = 1u; i < d->site_count; ++i)
        if (memcmp(d->sites[i].frames, frames,
                   H2_DIAG_FRAMES * sizeof(*frames)) == 0)
            return i;
    if (d->site_count >= d->config.site_capacity) {
        ++d->site_overflow;
        return 0u;
    }
    const size_t index = d->site_count++;
    memcpy(d->sites[index].frames, frames,
           H2_DIAG_FRAMES * sizeof(*frames));
    return index;
}

static void remove_tracked(h2_mem_arena_diagnostics_t *d,
                           const diag_block_t *b) {
    diag_tag_t *tag = &d->tags[b->tag];
    diag_site_t *site = &d->sites[b->site];
    tag->live -= b->requested;
    tag->consumed -= b->consumed;
    site->live -= b->requested;
    site->consumed -= b->consumed;
}
static void account_remove(h2_mem_arena_diagnostics_t *d,
                            const h2_mem_arena_block_info_t *info) {
    d->live -= info->requested_bytes;
    d->consumed -= info->consumed_bytes;
    if (info->fallback)
        d->fallback_live -= info->requested_bytes;
}
static void account_add(h2_mem_arena_diagnostics_t *d,
                         const h2_mem_arena_block_info_t *info) {
    d->live += info->requested_bytes;
    d->consumed += info->consumed_bytes;
    if (info->fallback)
        d->fallback_live += info->requested_bytes;
    if (d->consumed > d->consumed_peak)
        d->consumed_peak = d->consumed;
}
static void record_block(h2_mem_arena_diagnostics_t *d, void *ptr,
                          size_t tag_index, size_t site_index,
                          const h2_mem_arena_block_info_t *info) {
    const size_t index = empty_block_slot(d);
    if (index == SIZE_MAX) {
        ++d->block_overflow;
        return;
    }
    diag_block_t *b = &d->blocks[index];
    *b = (diag_block_t){.ptr = ptr, .id = ++d->next_id,
        .requested = info->requested_bytes, .consumed = info->consumed_bytes,
        .tag = tag_index, .site = site_index, .pool = info->pool,
        .fallback = info->fallback};
    diag_tag_t *tag = &d->tags[tag_index];
    diag_site_t *site = &d->sites[site_index];
    tag->live += b->requested;
    tag->consumed += b->consumed;
    ++tag->allocations;
    if (b->requested > tag->largest)
        tag->largest = b->requested;
    if (tag->live > tag->peak)
        tag->peak = tag->live;
    if (tag->consumed - tag->live > tag->overhead_peak)
        tag->overhead_peak = tag->consumed - tag->live;
    site->live += b->requested;
    site->consumed += b->consumed;
    ++site->requests;
    site->requested += b->requested;
    if (site->live > site->peak)
        site->peak = site->live;
    if (site->consumed - site->live > site->overhead_peak)
        site->overhead_peak = site->consumed - site->live;
}
static void maybe_peak(h2_mem_arena_diagnostics_t *d) {
    ++d->sequence;
    if (d->live <= d->peak.live)
        return;
    d->peak = (diag_peak_t){0};
    d->peak.seq = d->sequence;
    d->peak.live = d->live;
    d->peak.consumed = d->consumed;
    d->peak.fallback = d->fallback_live;
    for (size_t i = 0u; i < d->config.tag_count; ++i) {
        d->peak.tags[i] = d->tags[i].live;
        d->peak.consumed_tags[i] = d->tags[i].consumed;
    }
    if (d->peak_count == d->config.peak_capacity) {
        memmove(d->peaks, d->peaks + 1u,
                (d->peak_count - 1u) * sizeof(*d->peaks));
        --d->peak_count;
        ++d->peak_overflow;
    }
    d->peaks[d->peak_count++] = d->peak;
    if (d->config.trace && d->peak_blocks != NULL) {
        d->peak_block_count = 0u;
        for (size_t i = 0u; i < d->config.block_capacity; ++i) {
            const diag_block_t *b = &d->blocks[i];
            if (b->ptr == NULL)
                continue;
            size_t pos = 0u;
            while (pos < d->peak_block_count &&
                   d->peak_blocks[pos].requested >= b->requested)
                ++pos;
            if (pos >= d->config.top_count)
                continue;
            if (d->peak_block_count < d->config.top_count)
                ++d->peak_block_count;
            memmove(&d->peak_blocks[pos + 1u], &d->peak_blocks[pos],
                    (d->peak_block_count - pos - 1u) * sizeof(*b));
            d->peak_blocks[pos] = *b;
        }
    }
}

/* Call only while d->lock excludes free/realloc. The probe itself must copy
 * concurrent application writes without a C-language data race. */
static bool untouched_tail(h2_mem_arena_diagnostics_t *d, const void *ptr,
                            size_t bytes, size_t *out_unused) {
    *out_unused = 0u;
    if (d->config.probe.safe_copy == NULL || d->tail_scratch == NULL) {
        ++d->scan_unsupported;
        return false;
    }
    size_t remaining = bytes;
    while (remaining != 0u) {
        const size_t count = remaining < H2_DIAG_SCAN_CHUNK
                                 ? remaining : H2_DIAG_SCAN_CHUNK;
        const size_t offset = remaining - count;
        if (d->config.probe.safe_copy(
                d->config.probe.user,
                (const unsigned char *)ptr + offset,
                d->tail_scratch, count) != H2_PAL_OK)
            return false;
        for (size_t i = count; i != 0u; --i) {
            if (d->tail_scratch[i - 1u] != H2_DIAG_PATTERN)
                return true;
            ++*out_unused;
        }
        remaining = offset;
    }
    return true;
}

static h2_pal_result_t probe_site(void *user, uintptr_t *caller,
                                  uintptr_t *outer) {
    h2_mem_arena_diagnostics_t *d = user;
    uintptr_t frames[H2_DIAG_FRAMES] = {0};
    if (capture_site(d, frames) == 0u)
        return H2_PAL_ERR_UNSUPPORTED;
    *caller = frames[0];
    *outer = frames[1];
    return H2_PAL_OK;
}

static void *diag_alloc(void *user, size_t bytes) {
    diag_view_t *view = user;
    h2_mem_arena_diagnostics_t *d = view->owner;
    uintptr_t frames[H2_DIAG_FRAMES];
    (void)capture_site(d, frames);
    diag_lock(d, d->lock);
    const h2_pal_mem_api_t *inner = business_mem(d, view->tag);
    void *ptr = h2_pal_mem_alloc(inner, bytes);
    if (ptr != NULL) {
        if (d->config.trace && view->tag != d->config.stack_tag_index)
            memset(ptr, H2_DIAG_PATTERN, bytes);
        const h2_mem_arena_block_info_t info = block_info(d, ptr);
        account_add(d, &info);
        record_block(d, ptr, view->tag, site_slot(d, frames), &info);
        maybe_peak(d);
    } else if (bytes != 0u) {
        ++d->tags[view->tag].failures;
    }
    diag_unlock(d, d->lock);
    return ptr;
}

static void diag_free(void *user, void *ptr) {
    if (ptr == NULL)
        return;
    diag_view_t *view = user;
    h2_mem_arena_diagnostics_t *d = view->owner;
    diag_lock(d, d->lock);
    const h2_mem_arena_block_info_t info = block_info(d, ptr);
    const size_t index = block_slot(d, ptr);
    const size_t owner = index != SIZE_MAX ? d->blocks[index].tag : view->tag;
    if (index != SIZE_MAX) {
        diag_block_t b = d->blocks[index];
        if (d->config.trace && owner != d->config.stack_tag_index) {
            size_t unused = 0u;
            if (untouched_tail(d, ptr, b.requested, &unused)) {
                d->tags[owner].freed_requested += b.requested;
                d->tags[owner].freed_unused += unused;
                d->sites[b.site].freed_requested += b.requested;
                d->sites[b.site].freed_unused += unused;
            }
        }
        remove_tracked(d, &b);
        d->blocks[index].ptr = NULL;
    }
    account_remove(d, &info);
    ++d->sequence;
    h2_pal_mem_free(business_mem(d, owner), ptr);
    diag_unlock(d, d->lock);
}

static void *diag_realloc(void *user, void *ptr, size_t bytes) {
    if (ptr == NULL)
        return diag_alloc(user, bytes);
    if (bytes == 0u) {
        diag_free(user, ptr);
        return NULL;
    }
    diag_view_t *view = user;
    h2_mem_arena_diagnostics_t *d = view->owner;
    uintptr_t frames[H2_DIAG_FRAMES];
    (void)capture_site(d, frames);
    diag_lock(d, d->lock);
    const h2_mem_arena_block_info_t old_info = block_info(d, ptr);
    const size_t index = block_slot(d, ptr);
    const size_t owner = index != SIZE_MAX ? d->blocks[index].tag : view->tag;
    const diag_block_t old = index != SIZE_MAX ? d->blocks[index]
                                                 : (diag_block_t){0};
    size_t retired_unused = 0u;
    const bool retired_valid = index != SIZE_MAX && d->config.trace &&
        owner != d->config.stack_tag_index &&
        untouched_tail(d, ptr, old.requested, &retired_unused);
    void *next = h2_pal_mem_realloc(business_mem(d, owner), ptr, bytes);
    if (next == NULL) {
        ++d->tags[owner].failures;
        diag_unlock(d, d->lock);
        return NULL;
    }
    if (d->config.trace && owner != d->config.stack_tag_index &&
        bytes > old_info.requested_bytes)
        memset((unsigned char *)next + old_info.requested_bytes,
               H2_DIAG_PATTERN, bytes - old_info.requested_bytes);
    const h2_mem_arena_block_info_t info = block_info(d, next);
    account_remove(d, &old_info);
    if (index != SIZE_MAX) {
        if (retired_valid) {
            d->sites[old.site].retired_requested += old.requested;
            d->sites[old.site].retired_unused += retired_unused;
        }
        ++d->sites[old.site].realloc_count;
        d->sites[old.site].growth_bytes +=
            bytes > old.requested ? bytes - old.requested : 0u;
        d->sites[old.site].moved += next != ptr;
        remove_tracked(d, &old);
        d->blocks[index].ptr = NULL;
    }
    account_add(d, &info);
    record_block(d, next, owner, site_slot(d, frames), &info);
    if (next != ptr && d->consumed + old_info.consumed_bytes > d->overlap_upper)
        d->overlap_upper = d->consumed + old_info.consumed_bytes;
    maybe_peak(d);
    diag_unlock(d, d->lock);
    return next;
}

static const h2_pal_mem_vtable_t k_diag_vtable = {
    .alloc = diag_alloc, .realloc = diag_realloc, .free = diag_free};

static h2_pal_result_t write_bytes(h2_mem_arena_diagnostics_t *d,
                                    const void *data, size_t bytes) {
    if (d->file == NULL)
        return H2_PAL_ERR_UNSUPPORTED;
    const unsigned char *cursor = data;
    while (bytes != 0u) {
        size_t written = 0u;
        const h2_pal_result_t rc = (h2_pal_result_t)h2_pal_fs_write(
            d->config.report_fs, d->file, cursor, bytes, &written);
        if (rc != H2_PAL_OK)
            return rc;
        if (written == 0u || written > bytes)
            return H2_PAL_ERR_IO;
        cursor += written;
        bytes -= written;
    }
    return H2_PAL_OK;
}
static h2_pal_result_t writef(h2_mem_arena_diagnostics_t *d,
                               const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (count < 0)
        return H2_PAL_ERR_FORMAT;
    if ((size_t)count >= sizeof(line))
        return H2_PAL_ERR_TRUNCATED;
    return write_bytes(d, line, (size_t)count);
}
static void checked_writef(h2_mem_arena_diagnostics_t *d,
                            const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    h2_pal_result_t rc = H2_PAL_OK;
    if (count < 0)
        rc = H2_PAL_ERR_FORMAT;
    else if ((size_t)count >= sizeof(line))
        rc = H2_PAL_ERR_TRUNCATED;
    else
        rc = write_bytes(d, line, (size_t)count);
    if (rc != H2_PAL_OK && d->report_result == H2_PAL_OK)
        d->report_result = rc;
}
static void diag_log(h2_mem_arena_diagnostics_t *d,
                      h2_pal_log_level_t level, const char *format, ...) {
    if (d->config.log == NULL)
        return;
    char message[512];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (count >= 0 && (size_t)count < sizeof(message))
        (void)h2_pal_log_write(d->config.log, level,
            d->config.log_scope != NULL ? d->config.log_scope : "arena",
            message);
}
static void free_snapshot(h2_mem_arena_diagnostics_t *d,
                           diag_snapshot_t *snapshot) {
    if (snapshot == NULL)
        return;
    h2_pal_mem_free(d->config.metadata, snapshot->blocks);
    h2_pal_mem_free(d->config.metadata, snapshot->top_blocks);
    h2_pal_mem_free(d->config.metadata, snapshot->sites);
    h2_pal_mem_free(d->config.metadata, snapshot->peaks);
    h2_pal_mem_free(d->config.metadata, snapshot);
}
static void copy_census(void *user,
                         const h2_mem_arena_census_snapshot_t *current) {
    diag_snapshot_t *snapshot = user;
    for (size_t i = 0u; i < current->tag_count && i < H2_DIAG_MAX_TAGS; ++i) {
        snapshot->census_live[i] = current->tags[i].counts.live_bytes;
        snapshot->census_peak[i] = current->tags[i].counts.peak_bytes;
    }
    snapshot->census_unattributed_live =
        current->unattributed_overflow.live_bytes;
    snapshot->census_block_overflow = current->block_overflow;
    snapshot->census_site_overflow = current->site_overflow;
}
static bool is_case_end(const h2_mem_arena_diagnostics_t *d,
                         const char *phase) {
    const char *prefix = d->config.case_end_prefix;
    return prefix != NULL && strncmp(phase, prefix, strlen(prefix)) == 0;
}
static void update_case_end(h2_mem_arena_diagnostics_t *d,
                             const diag_snapshot_t *snapshot) {
    if (d->case_ends == 0u) {
        d->resident_count = snapshot->block_count;
        for (size_t i = 0u; i < d->resident_count; ++i)
            d->resident_ids[i] = snapshot->blocks[i].id;
    } else {
        size_t next = 0u;
        for (size_t i = 0u; i < d->resident_count; ++i) {
            const uint64_t id = d->resident_ids[i];
            bool present = false;
            for (size_t j = 0u; j < snapshot->block_count; ++j)
                if (snapshot->blocks[j].id == id) {
                    present = true;
                    break;
                }
            if (present)
                d->resident_ids[next++] = id;
        }
        d->resident_count = next;
    }
    for (size_t i = 0u; i < d->config.tag_count; ++i) {
        diag_tag_t *tag = &d->tags[i];
        if (d->case_ends == 0u)
            tag->end_first = tag->end_min = tag->end_max = tag->live;
        tag->end_last = tag->live;
        if (tag->live < tag->end_min)
            tag->end_min = tag->live;
        if (tag->live > tag->end_max)
            tag->end_max = tag->live;
    }
    ++d->case_ends;
}
static diag_snapshot_t *capture_snapshot(h2_mem_arena_diagnostics_t *d,
                                          const char *phase) {
    diag_snapshot_t *s = metadata_alloc(d, 1u, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->blocks = metadata_alloc(d, d->config.block_capacity, sizeof(*s->blocks));
    s->top_blocks = metadata_alloc(d, d->config.top_count,
                                    sizeof(*s->top_blocks));
    s->sites = metadata_alloc(d, d->config.site_capacity, sizeof(*s->sites));
    s->peaks = metadata_alloc(d, d->config.peak_capacity, sizeof(*s->peaks));
    if (s->blocks == NULL || s->top_blocks == NULL || s->sites == NULL ||
        s->peaks == NULL) {
        free_snapshot(d, s);
        return NULL;
    }
    size_t length = strlen(phase);
    if (length >= sizeof(s->phase))
        length = sizeof(s->phase) - 1u;
    for (size_t i = 0u; i < length; ++i) {
        const unsigned char c = (unsigned char)phase[i];
        s->phase[i] = c <= ' ' || c == '=' ? '_' : (char)c;
    }
    s->phase[length] = '\0';
    uint64_t capture_started_us = 0u;
    (void)h2_pal_time_get_monotonic_us(d->config.time, &capture_started_us);
    diag_lock(d, d->lock);
    s->seq = ++d->snapshot_sequence;
    s->allocation_seq = d->sequence;
    s->live = d->live;
    s->consumed = d->consumed;
    s->block_overflow = d->block_overflow;
    s->site_overflow = d->site_overflow;
    s->peak_overflow = d->peak_overflow;
    s->report_backpressure = d->report_backpressure;
    s->peak = d->peak;
    s->site_count = d->site_count;
    s->peak_count = d->peak_count;
    s->top_count = d->peak_block_count;
    memcpy(s->tags, d->tags, sizeof(s->tags));
    memcpy(s->sites, d->sites, s->site_count * sizeof(*s->sites));
    memcpy(s->peaks, d->peaks, s->peak_count * sizeof(*s->peaks));
    memcpy(s->top_blocks, d->peak_blocks,
           s->top_count * sizeof(*s->top_blocks));
    for (size_t i = 0u; i < d->config.block_capacity; ++i)
        if (d->blocks[i].ptr != NULL)
            s->blocks[s->block_count++] = d->blocks[i];
    (void)h2_mem_arena_inspect(d->arena, s->pools);
    (void)h2_mem_arena_stats(d->arena, &s->arena_stats);
    if (d->census != NULL)
        (void)h2_mem_arena_census_snapshot(d->census, copy_census, s);
    (void)h2_pal_time_get_monotonic_us(d->config.time, &s->captured_us);
    s->capture_elapsed_us = s->captured_us >= capture_started_us
                                ? s->captured_us - capture_started_us : 0u;
    if (is_case_end(d, s->phase))
        update_case_end(d, s);
    s->case_ends = d->case_ends;
    diag_unlock(d, d->lock);
    return s;
}

typedef struct diag_sample {
    bool valid;
    size_t unused;
    uint64_t hash;
} diag_sample_t;

static diag_sample_t scan_block(h2_mem_arena_diagnostics_t *d,
                                 const diag_block_t *b) {
    diag_sample_t result = {.hash = UINT64_C(14695981039346656037)};
    if (b->tag == d->config.stack_tag_index ||
        d->config.probe.safe_copy == NULL || d->report_scratch == NULL)
        return result;
    size_t used = 0u;
    uint64_t used_hash = result.hash;
    for (size_t offset = 0u; offset < b->requested;
         offset += H2_DIAG_SCAN_CHUNK) {
        const size_t count = b->requested - offset < H2_DIAG_SCAN_CHUNK
                                 ? b->requested - offset : H2_DIAG_SCAN_CHUNK;
        diag_lock(d, d->lock);
        const size_t slot = block_slot(d, b->ptr);
        const bool current = slot != SIZE_MAX &&
            d->blocks[slot].id == b->id;
        const h2_pal_result_t rc = current
            ? d->config.probe.safe_copy(d->config.probe.user,
                (const unsigned char *)b->ptr + offset,
                d->report_scratch, count)
            : H2_PAL_ERR_CLOSED;
        diag_unlock(d, d->lock);
        if (rc != H2_PAL_OK)
            return result;
        for (size_t i = 0u; i < count; ++i) {
            if (b->requested >= H2_DIAG_LARGE) {
                result.hash ^= d->report_scratch[i];
                result.hash *= UINT64_C(1099511628211);
            }
            if (d->report_scratch[i] != H2_DIAG_PATTERN) {
                used = offset + i + 1u;
                used_hash = result.hash;
            }
        }
    }
    result.hash = used_hash;
    result.unused = b->requested - used;
    result.valid = true;
    return result;
}

static bool still_live(h2_mem_arena_diagnostics_t *d,
                        const diag_block_t *b) {
    diag_lock(d, d->lock);
    const size_t slot = block_slot(d, b->ptr);
    const bool live = slot != SIZE_MAX && d->blocks[slot].id == b->id;
    diag_unlock(d, d->lock);
    return live;
}

static void remember_duplicate(h2_mem_arena_diagnostics_t *d,
                                const diag_block_t *a,
                                const diag_block_t *b, size_t used,
                                uint64_t seq, uint64_t hash) {
    const uint64_t first = a->id < b->id ? a->id : b->id;
    const uint64_t second = a->id < b->id ? b->id : a->id;
    for (size_t i = 0u; i < d->duplicate_count; ++i)
        if (d->duplicates[i].first == first &&
            d->duplicates[i].second == second) {
            if (used > d->duplicates[i].bytes)
                d->duplicates[i] = (diag_duplicate_t){first, second, seq, used};
            return;
        }
    if (d->duplicate_count == d->config.duplicate_capacity)
        ++d->duplicate_overflow;
    else
        d->duplicates[d->duplicate_count++] =
            (diag_duplicate_t){first, second, seq, used};
    checked_writef(d,
        "DUPLICATE seq=%llu id=%llu other=%llu bytes=%zu capacity=%zu "
        "other_capacity=%zu hash=%llu kind=hash_candidate\n",
        (unsigned long long)seq, (unsigned long long)b->id,
        (unsigned long long)a->id, used, b->requested, a->requested,
        (unsigned long long)hash);
}

static void render_snapshot(h2_mem_arena_diagnostics_t *d,
                             const diag_snapshot_t *s) {
    uint64_t started_us = 0u;
    (void)h2_pal_time_get_monotonic_us(d->config.time, &started_us);
    for (size_t i = 0u; i < s->site_count; ++i) {
        const diag_site_t *site = &s->sites[i];
        checked_writef(d,
            "SITE id=%zu frame0=%llu frame1=%llu frame2=%llu frame3=%llu "
            "frame4=%llu frame5=%llu frame6=%llu frame7=%llu\n", i,
            (unsigned long long)site->frames[0],
            (unsigned long long)site->frames[1],
            (unsigned long long)site->frames[2],
            (unsigned long long)site->frames[3],
            (unsigned long long)site->frames[4],
            (unsigned long long)site->frames[5],
            (unsigned long long)site->frames[6],
            (unsigned long long)site->frames[7]);
    }
    for (size_t i = 0u; i < s->peak_count; ++i) {
        const diag_peak_t *peak = &s->peaks[i];
        checked_writef(d,
            "PEAK seq=%llu requested=%zu consumed=%zu fallback=%zu\n",
            (unsigned long long)peak->seq, peak->live, peak->consumed,
            peak->fallback);
        for (size_t tag = 0u; tag < d->config.tag_count; ++tag)
            checked_writef(d,
                "PEAK_TAG seq=%llu tag=%s live=%zu consumed=%zu\n",
                (unsigned long long)peak->seq, d->config.tags[tag],
                peak->tags[tag], peak->consumed_tags[tag]);
    }
    size_t census_live = s->census_unattributed_live;
    for (size_t i = 0u; i < d->config.tag_count; ++i)
        census_live += s->census_live[i];
    checked_writef(d,
        "SNAPSHOT phase=%s seq=%llu allocation_seq=%llu live=%zu "
        "census_live=%zu consumed=%zu peak=%zu small_free=%zu "
        "small_largest=%zu large_free=%zu large_largest=%zu fallback=%zu\n",
        s->phase, (unsigned long long)s->seq,
        (unsigned long long)s->allocation_seq, s->live, census_live,
        s->consumed, s->peak.live, s->pools[0].free_bytes,
        s->pools[0].largest_free_block, s->pools[1].free_bytes,
        s->pools[1].largest_free_block,
        s->arena_stats.fallback_live_bytes);
    size_t unused[H2_DIAG_MAX_TAGS] = {0};
    size_t scanned[H2_DIAG_MAX_TAGS] = {0};
    memset(d->site_unused_scratch, 0,
           d->config.site_capacity * sizeof(*d->site_unused_scratch));
    size_t hash_count = 0u, skipped = 0u, duplicates = 0u;
    for (size_t i = 0u; d->config.trace && i < s->block_count; ++i) {
        const diag_block_t *b = &s->blocks[i];
        if (b->tag == d->config.stack_tag_index)
            continue;
        const diag_sample_t sample = scan_block(d, b);
        if (!sample.valid) {
            ++skipped;
            continue;
        }
        unused[b->tag] += sample.unused;
        scanned[b->tag] += b->requested;
        d->site_unused_scratch[b->site] += sample.unused;
        checked_writef(d,
            "BLOCK id=%llu tag=%s requested=%zu consumed=%zu overhead=%zu "
            "pool=%u fallback=%u site=%zu seq=%llu unused=%zu "
            "used_estimate=%zu\n", (unsigned long long)b->id,
            d->config.tags[b->tag], b->requested, b->consumed,
            b->consumed - b->requested, b->pool, b->fallback ? 1u : 0u,
            b->site, (unsigned long long)s->seq, sample.unused,
            b->requested - sample.unused);
        const size_t used = b->requested - sample.unused;
        if (used < H2_DIAG_LARGE)
            continue;
        bool matched = false;
        for (size_t j = 0u; j < hash_count; ++j)
            if (d->hash_samples[j].used == used &&
                d->hash_samples[j].hash == sample.hash &&
                still_live(d, &d->hash_samples[j].block) &&
                still_live(d, b)) {
                remember_duplicate(d, &d->hash_samples[j].block, b,
                                   used, s->seq, sample.hash);
                duplicates += used;
                matched = true;
                break;
            }
        if (!matched && hash_count < d->config.duplicate_capacity)
            d->hash_samples[hash_count++] =
                (diag_hash_sample_t){used, sample.hash, *b};
        else if (!matched)
            ++d->duplicate_overflow;
    }
    for (size_t i = 0u; i < s->top_count; ++i) {
        const diag_block_t *b = &s->top_blocks[i];
        checked_writef(d,
            "PEAK_BLOCK peak_seq=%llu snapshot_seq=%llu id=%llu tag=%s "
            "requested=%zu consumed=%zu overhead=%zu pool=%u fallback=%u "
            "site=%zu\n", (unsigned long long)s->peak.seq,
            (unsigned long long)s->seq, (unsigned long long)b->id,
            d->config.tags[b->tag], b->requested, b->consumed,
            b->consumed - b->requested, b->pool, b->fallback ? 1u : 0u,
            b->site);
    }
    for (size_t i = 0u; i < d->config.tag_count; ++i) {
        const diag_tag_t *tag = &s->tags[i];
        checked_writef(d,
            "TAG seq=%llu tag=%s live=%zu peak=%zu allocations=%zu "
            "failures=%zu largest=%zu consumed=%zu census_live=%zu "
            "census_peak=%zu scanned=%zu unused=%zu scan_exempt=%u\n",
            (unsigned long long)s->seq, d->config.tags[i], tag->live,
            tag->peak, tag->allocations, tag->failures, tag->largest,
            tag->consumed, s->census_live[i], s->census_peak[i], scanned[i],
            unused[i], i == d->config.stack_tag_index ? 1u : 0u);
    }
    diag_lock(d, d->lock);
    for (size_t i = 0u; i < d->config.tag_count; ++i)
        if (unused[i] > d->tags[i].unused_max)
            d->tags[i].unused_max = unused[i];
    for (size_t i = 0u; i < s->site_count; ++i)
        if (d->site_unused_scratch[i] > d->sites[i].unused_snapshot_max)
            d->sites[i].unused_snapshot_max = d->site_unused_scratch[i];
    diag_unlock(d, d->lock);
    uint64_t ended_us = 0u;
    (void)h2_pal_time_get_monotonic_us(d->config.time, &ended_us);
    checked_writef(d,
        "SCAN seq=%llu skipped=%zu duplicate_candidate_bytes=%zu "
        "elapsed_us=%llu capture_us=%llu queue_us=%llu\n",
        (unsigned long long)s->seq, skipped, duplicates,
        (unsigned long long)(ended_us >= started_us ? ended_us - started_us : 0u),
        (unsigned long long)s->capture_elapsed_us,
        (unsigned long long)(s->captured_us != 0u && started_us >= s->captured_us
                                 ? started_us - s->captured_us : 0u));
    checked_writef(d,
        "OVERFLOW seq=%llu blocks=%zu sites=%zu peaks=%zu "
        "duplicates=%zu report_backpressure=%zu census_blocks=%zu "
        "census_sites=%zu census_unattributed_live=%zu "
        "content_probe_unsupported=%zu\n", (unsigned long long)s->seq,
        s->block_overflow, s->site_overflow, s->peak_overflow,
        d->duplicate_overflow, s->report_backpressure,
        s->census_block_overflow, s->census_site_overflow,
        s->census_unattributed_live, d->scan_unsupported);
    if (d->file != NULL) {
        const h2_pal_result_t rc = (h2_pal_result_t)h2_pal_fs_sync(
            d->config.report_fs, d->file);
        if (rc != H2_PAL_OK && d->report_result == H2_PAL_OK)
            d->report_result = rc;
    }
    diag_log(d, H2_PAL_LOG_INFO,
             "phase=%s live=%zu peak=%zu small_live=%zu large_live=%zu "
             "fallback=%zu small_free=%zu small_largest=%zu large_free=%zu "
             "large_largest=%zu scan_us=%llu", s->phase, s->live,
             s->peak.live, s->arena_stats.small.live_bytes,
             s->arena_stats.large.live_bytes,
             s->arena_stats.fallback_live_bytes, s->pools[0].free_bytes,
             s->pools[0].largest_free_block, s->pools[1].free_bytes,
             s->pools[1].largest_free_block,
             (unsigned long long)(ended_us >= started_us
                                      ? ended_us - started_us : 0u));
}

static int duplicate_largest_first(const void *left, const void *right) {
    const diag_duplicate_t *a = left, *b = right;
    return a->bytes < b->bytes ? 1 : a->bytes > b->bytes ? -1 : 0;
}
static size_t ranked_tag_value(const h2_mem_arena_diagnostics_t *d,
                                size_t tag, unsigned category,
                                const size_t retained[H2_DIAG_MAX_TAGS]) {
    const diag_tag_t *t = &d->tags[tag];
    if (category == 0u)
        return retained[tag];
    if (category == 1u)
        return t->end_last > t->end_first
                   ? t->end_last - t->end_first : 0u;
    if (category == 2u)
        return t->unused_max;
    return t->overhead_peak;
}
static void summarize(h2_mem_arena_diagnostics_t *d) {
    size_t retained[H2_DIAG_MAX_TAGS] = {0};
    for (size_t i = 0u; i < d->resident_count; ++i)
        for (size_t j = 0u; j < d->config.block_capacity; ++j)
            if (d->blocks[j].ptr != NULL &&
                d->blocks[j].id == d->resident_ids[i]) {
                retained[d->blocks[j].tag] += d->blocks[j].requested;
                break;
            }
    for (size_t tag = 0u; tag < d->config.tag_count; ++tag) {
        const diag_tag_t *t = &d->tags[tag];
        checked_writef(d,
            "SUMMARY_TAG tag=%s at_total_peak=%zu "
            "consumed_at_total_peak=%zu peak_share_pct=%zu own_peak=%zu "
            "overhead_peak=%zu live_at_exit=%zu allocations=%zu failures=%zu "
            "largest=%zu case_ends=%llu resident_same_blocks=%zu "
            "end_min=%zu end_max=%zu end_growth=%lld stable_end_bytes=%zu "
            "unused_snapshot_max=%zu freed_requested=%zu freed_unused=%zu\n",
            d->config.tags[tag], d->peak.tags[tag],
            d->peak.consumed_tags[tag],
            d->peak.live != 0u ? 100u * d->peak.tags[tag] / d->peak.live : 0u,
            t->peak, t->overhead_peak, t->live, t->allocations, t->failures,
            t->largest, (unsigned long long)d->case_ends, retained[tag],
            t->end_min, t->end_max,
            (long long)t->end_last - (long long)t->end_first,
            d->case_ends > 1u && t->end_min == t->end_max ? t->end_min : 0u,
            t->unused_max, t->freed_requested, t->freed_unused);
    }
    for (size_t i = 0u; i < d->site_count; ++i) {
        const diag_site_t *s = &d->sites[i];
        checked_writef(d,
            "SITE id=%zu requests=%zu requested=%zu freed_requested=%zu "
            "freed_unused=%zu live=%zu own_peak=%zu retired_requested=%zu "
            "retired_unused=%zu reallocs=%zu growth_bytes=%zu moved=%zu "
            "frame0=%llu frame1=%llu frame2=%llu frame3=%llu frame4=%llu "
            "frame5=%llu frame6=%llu frame7=%llu\n", i, s->requests,
            s->requested, s->freed_requested, s->freed_unused, s->live,
            s->peak, s->retired_requested, s->retired_unused,
            s->realloc_count, s->growth_bytes, s->moved,
            (unsigned long long)s->frames[0],
            (unsigned long long)s->frames[1],
            (unsigned long long)s->frames[2],
            (unsigned long long)s->frames[3],
            (unsigned long long)s->frames[4],
            (unsigned long long)s->frames[5],
            (unsigned long long)s->frames[6],
            (unsigned long long)s->frames[7]);
    }
    const char *const categories[] = {
        "resident", "growth", "unused", "overhead"};
    for (unsigned category = 0u; category < 4u; ++category) {
        bool used[H2_DIAG_MAX_TAGS] = {false};
        for (size_t rank = 0u; rank < d->config.tag_count; ++rank) {
            size_t best = SIZE_MAX, value = 0u;
            for (size_t tag = 0u; tag < d->config.tag_count; ++tag) {
                const size_t candidate =
                    ranked_tag_value(d, tag, category, retained);
                if (!used[tag] && (best == SIZE_MAX || candidate > value)) {
                    best = tag;
                    value = candidate;
                }
            }
            used[best] = true;
            checked_writef(d, "RANK category=%s tag=%s bytes=%zu\n",
                           categories[category], d->config.tags[best], value);
        }
    }
    for (size_t i = 0u; i < d->site_count; ++i)
        d->site_rank_scratch[i] = i;
    for (size_t i = 0u; i < d->site_count; ++i) {
        size_t best = i;
        for (size_t j = i + 1u; j < d->site_count; ++j)
            if (d->sites[d->site_rank_scratch[j]].peak >
                d->sites[d->site_rank_scratch[best]].peak)
                best = j;
        const size_t saved = d->site_rank_scratch[i];
        d->site_rank_scratch[i] = d->site_rank_scratch[best];
        d->site_rank_scratch[best] = saved;
        const size_t site = d->site_rank_scratch[i];
        checked_writef(d, "RANK_SITE site=%zu own_peak=%zu\n",
                       site, d->sites[site].peak);
    }
    qsort(d->duplicates, d->duplicate_count, sizeof(*d->duplicates),
          duplicate_largest_first);
    for (size_t i = 0u; i < d->duplicate_count; ++i)
        checked_writef(d,
            "RANK_DUPLICATE id=%llu other=%llu bytes=%zu snapshot=%llu\n",
            (unsigned long long)d->duplicates[i].first,
            (unsigned long long)d->duplicates[i].second,
            d->duplicates[i].bytes,
            (unsigned long long)d->duplicates[i].snapshot);
    checked_writef(d,
        "END live=%zu peak=%zu consumed_peak=%zu "
        "moved_realloc_overlap_upper=%zu blocks=%zu\n",
        d->live, d->peak.live, d->consumed_peak,
        d->overlap_upper, d->resident_count);
    if (d->file != NULL)
        (void)h2_pal_fs_sync(d->config.report_fs, d->file);
}

static void reporter_entry(void *user) {
    h2_mem_arena_diagnostics_t *d = user;
    for (;;) {
        diag_snapshot_t *snapshot = NULL;
        const h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_recv(
            d->config.queue, d->reports, &snapshot,
            H2_PAL_QUEUE_WAIT_FOREVER);
        if (rc != H2_PAL_OK)
            return;
        if (snapshot == NULL) {
            (void)h2_pal_semaphore_give(d->config.sync, d->flush_done);
            continue;
        }
        render_snapshot(d, snapshot);
        free_snapshot(d, snapshot);
    }
}

static void release_resources(h2_mem_arena_diagnostics_t *d) {
    if (d == NULL)
        return;
    if (d->file != NULL)
        (void)h2_pal_fs_close(d->config.report_fs, d->file);
    if (d->reports != NULL)
        h2_pal_queue_destroy(d->config.queue, d->reports);
    if (d->flush_done != NULL)
        (void)h2_pal_semaphore_destroy(d->config.sync, d->flush_done);
    if (d->census != NULL)
        (void)h2_mem_arena_census_destroy(d->census);
    if (d->arena != NULL)
        (void)h2_mem_arena_destroy(d->arena);
    if (d->report_lock != NULL)
        (void)h2_pal_mutex_destroy(d->config.sync, d->report_lock);
    if (d->lock != NULL)
        (void)h2_pal_mutex_destroy(d->config.sync, d->lock);
    if (d->core_lock != NULL)
        (void)h2_pal_mutex_destroy(d->config.sync, d->core_lock);
    const h2_pal_mem_api_t *metadata = d->config.metadata;
    h2_pal_mem_free(metadata, d->census_tags);
    h2_pal_mem_free(metadata, d->views);
    h2_pal_mem_free(metadata, d->blocks);
    h2_pal_mem_free(metadata, d->sites);
    h2_pal_mem_free(metadata, d->peaks);
    h2_pal_mem_free(metadata, d->duplicates);
    h2_pal_mem_free(metadata, d->hash_samples);
    h2_pal_mem_free(metadata, d->peak_blocks);
    h2_pal_mem_free(metadata, d->resident_ids);
    h2_pal_mem_free(metadata, d->tail_scratch);
    h2_pal_mem_free(metadata, d->report_scratch);
    h2_pal_mem_free(metadata, d->site_unused_scratch);
    h2_pal_mem_free(metadata, d->site_rank_scratch);
    h2_pal_mem_free(metadata, d);
}

static bool valid_config(const h2_mem_arena_diagnostics_config_t *c) {
    if (c == NULL || c->backing == NULL || c->backing_bytes == 0u ||
        c->metadata == NULL || c->metadata->vtable == NULL ||
        c->metadata->vtable->alloc == NULL ||
        c->metadata->vtable->free == NULL || c->sync == NULL ||
        c->sync->vtable == NULL || c->sync->vtable->create_mutex == NULL ||
        c->sync->vtable->destroy_mutex == NULL ||
        c->sync->vtable->lock_mutex == NULL ||
        c->sync->vtable->unlock_mutex == NULL || c->tags == NULL ||
        c->tag_count == 0u || c->tag_count > H2_DIAG_MAX_TAGS ||
        c->stack_tag_index >= c->tag_count ||
        c->block_capacity == 0u || c->site_capacity < 2u ||
        c->peak_capacity == 0u || c->duplicate_capacity == 0u ||
        c->top_count == 0u || c->top_count > c->block_capacity ||
        c->report_queue_capacity == 0u ||
        c->report_queue_capacity >= UINT32_MAX ||
        (c->task != NULL && c->queue == NULL))
        return false;
    for (size_t i = 0u; i < c->tag_count; ++i) {
        if (c->tags[i] == NULL || c->tags[i][0] == '\0')
            return false;
        for (size_t j = 0u; j < i; ++j)
            if (strcmp(c->tags[i], c->tags[j]) == 0)
                return false;
    }
    return true;
}

h2_pal_result_t h2_mem_arena_diagnostics_create(
    const h2_mem_arena_diagnostics_config_t *config,
    h2_mem_arena_diagnostics_t **out) {
    if (out == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (!valid_config(config))
        return H2_PAL_ERR_INVALID_ARG;
    h2_mem_arena_diagnostics_t *d =
        h2_pal_mem_alloc(config->metadata, sizeof(*d));
    if (d == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(d, 0, sizeof(*d));
    d->config = *config;
    d->report_result = H2_PAL_OK;
    h2_pal_mutex_config_t mutex_config = {
        .name = "arena/diagnostics", .allocator = config->metadata};
    h2_pal_result_t rc = h2_pal_mutex_create(config->sync, &mutex_config,
                                              &d->core_lock);
    if (rc == H2_PAL_OK)
        rc = h2_pal_mutex_create(config->sync, &mutex_config, &d->lock);
    if (rc == H2_PAL_OK)
        rc = h2_pal_mutex_create(config->sync, &mutex_config,
                                 &d->report_lock);
    if (rc != H2_PAL_OK) {
        release_resources(d);
        return rc;
    }
    h2_mem_arena_config_t arena_config = {
        .name = config->name, .block = config->backing,
        .block_bytes = config->backing_bytes,
        .small_request_max = config->small_request_max,
        .small_pool_bytes = config->small_pool_bytes,
        .fallback = config->fallback, .lock = arena_lock,
        .unlock = arena_unlock, .lock_user = d};
    rc = h2_mem_arena_create(&arena_config, &d->arena);
    if (rc != H2_PAL_OK) {
        release_resources(d);
        return rc;
    }
    d->census_tags = metadata_alloc(d, config->tag_count,
                                     sizeof(*d->census_tags));
    d->views = metadata_alloc(d, config->tag_count, sizeof(*d->views));
    d->blocks = metadata_alloc(d, config->block_capacity,
                                sizeof(*d->blocks));
    d->sites = metadata_alloc(d, config->site_capacity, sizeof(*d->sites));
    d->peaks = metadata_alloc(d, config->peak_capacity, sizeof(*d->peaks));
    d->duplicates = metadata_alloc(d, config->duplicate_capacity,
                                    sizeof(*d->duplicates));
    d->hash_samples = metadata_alloc(d, config->duplicate_capacity,
                                      sizeof(*d->hash_samples));
    d->peak_blocks = metadata_alloc(d, config->top_count,
                                     sizeof(*d->peak_blocks));
    d->resident_ids = metadata_alloc(d, config->block_capacity,
                                      sizeof(*d->resident_ids));
    d->site_unused_scratch = metadata_alloc(d, config->site_capacity,
                                             sizeof(*d->site_unused_scratch));
    d->site_rank_scratch = metadata_alloc(d, config->site_capacity,
                                           sizeof(*d->site_rank_scratch));
    if (config->trace) {
        d->tail_scratch = metadata_alloc(d, H2_DIAG_SCAN_CHUNK, 1u);
        d->report_scratch = metadata_alloc(d, H2_DIAG_SCAN_CHUNK, 1u);
    }
    if (d->census_tags == NULL || d->views == NULL || d->blocks == NULL ||
        d->sites == NULL || d->peaks == NULL || d->duplicates == NULL ||
        d->hash_samples == NULL || d->peak_blocks == NULL ||
        d->resident_ids == NULL || d->site_unused_scratch == NULL ||
        d->site_rank_scratch == NULL ||
        (config->trace && (d->tail_scratch == NULL ||
                           d->report_scratch == NULL))) {
        release_resources(d);
        return H2_PAL_ERR_NO_MEMORY;
    }
    d->site_count = 1u;
    for (size_t i = 0u; i < config->tag_count; ++i)
        d->census_tags[i] = (h2_mem_arena_census_tag_config_t){
            .name = config->tags[i],
            .track_sites = config->trace && i != config->stack_tag_index};
    h2_mem_arena_census_config_t census_config = {
        .arena = d->arena, .metadata = config->metadata, .sync = config->sync,
        .tags = d->census_tags, .tag_count = config->tag_count,
        .capture_site = probe_site, .capture_site_user = d,
        .block_capacity = config->block_capacity,
        .site_capacity = config->site_capacity,
        .probe_limit = config->block_capacity < config->site_capacity - 1u
                           ? config->block_capacity : config->site_capacity - 1u};
    if (census_config.probe_limit > 16u)
        census_config.probe_limit = 16u;
    rc = h2_mem_arena_census_create(&census_config, &d->census);
    if (rc != H2_PAL_OK) {
        release_resources(d);
        return rc;
    }
    for (size_t i = 0u; i < config->tag_count; ++i)
        d->views[i] = (diag_view_t){
            .api = {.user = &d->views[i], .vtable = &k_diag_vtable},
            .owner = d, .tag = i};
    if (config->report_fs != NULL && config->report_path != NULL) {
        rc = (h2_pal_result_t)h2_pal_fs_open(config->report_fs,
            config->report_path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &d->file);
        if (rc == H2_PAL_OK) {
            (void)writef(d,
                "META version=1 trace=%u reserved=%zu anchor=%llu "
                "anchor_symbol=%s content=concurrent_sample "
                "unused=pattern_tail_estimate fallback_consumed=lower_bound\n",
                config->trace ? 1u : 0u, config->backing_bytes,
                (unsigned long long)config->symbol_anchor,
                config->symbol_anchor_name != NULL
                    ? config->symbol_anchor_name : "none");
        } else {
            diag_log(d, H2_PAL_LOG_ERROR,
                     "report_open_failed rc=%d path=%s", rc,
                     config->report_path);
        }
    }
    if (config->task != NULL && config->queue != NULL &&
        config->sync->vtable->create_semaphore != NULL &&
        config->sync->vtable->take_semaphore != NULL &&
        config->sync->vtable->give_semaphore != NULL) {
        const h2_pal_queue_config_t queue_config = {
            .name = "arena/diagnostics", .item_size = sizeof(diag_snapshot_t *),
            .item_count = config->report_queue_capacity + 1u,
            .allocator = config->metadata};
        rc = (h2_pal_result_t)h2_pal_queue_create(config->queue,
                                                    &queue_config, &d->reports);
        h2_pal_semaphore_config_t sem_config = {
            .name = "arena/diagnostics-flush", .allocator = config->metadata,
            .initial_count = 0u, .max_count = 1u};
        if (rc == H2_PAL_OK)
            rc = h2_pal_semaphore_create(config->sync, &sem_config,
                                          &d->flush_done);
        if (rc == H2_PAL_OK) {
            const h2_pal_task_options_t options = {
                .name = config->report_task_name != NULL
                            ? config->report_task_name : "arena/diagnostics",
                .min_stack_size = config->report_task_stack_size != 0u
                                      ? config->report_task_stack_size : 32768u};
            rc = h2_pal_task_start(config->task, &options, reporter_entry,
                                    d, &d->reporter);
        }
        if (rc != H2_PAL_OK) {
            if (d->reports != NULL) {
                h2_pal_queue_destroy(config->queue, d->reports);
                d->reports = NULL;
            }
            if (d->flush_done != NULL) {
                (void)h2_pal_semaphore_destroy(config->sync, d->flush_done);
                d->flush_done = NULL;
            }
            diag_log(d, H2_PAL_LOG_WARN,
                     "report_worker_unavailable rc=%d mode=synchronous", rc);
        }
    }
    *out = d;
    return H2_PAL_OK;
}

const h2_pal_mem_api_t *h2_mem_arena_diagnostics_mem(
    h2_mem_arena_diagnostics_t *d, const char *tag) {
    if (d == NULL)
        return NULL;
    size_t index = 0u;
    if (tag != NULL)
        for (size_t i = 0u; i < d->config.tag_count; ++i)
            if (strcmp(tag, d->config.tags[i]) == 0) {
                index = i;
                break;
            }
    return &d->views[index].api;
}

h2_pal_result_t h2_mem_arena_diagnostics_stats(
    h2_mem_arena_diagnostics_t *d, h2_mem_arena_stats_t *out_stats) {
    if (out_stats == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    memset(out_stats, 0, sizeof(*out_stats));
    return d != NULL ? h2_mem_arena_stats(d->arena, out_stats)
                     : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_mem_arena_diagnostics_report(
    h2_mem_arena_diagnostics_t *d, const char *phase) {
    if (d == NULL || phase == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    diag_lock(d, d->report_lock);
    diag_snapshot_t *snapshot = capture_snapshot(d, phase);
    if (snapshot == NULL) {
        diag_unlock(d, d->report_lock);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (d->reporter != NULL) {
        h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_send(
            d->config.queue, d->reports, &snapshot, H2_PAL_QUEUE_NO_WAIT);
        if (rc == H2_PAL_ERR_FULL) {
            ++d->report_backpressure;
            rc = (h2_pal_result_t)h2_pal_queue_send(d->config.queue,
                d->reports, &snapshot, H2_PAL_QUEUE_WAIT_FOREVER);
        }
        if (rc != H2_PAL_OK) {
            free_snapshot(d, snapshot);
            diag_unlock(d, d->report_lock);
            return rc;
        }
        diag_unlock(d, d->report_lock);
        return H2_PAL_OK;
    } else {
        render_snapshot(d, snapshot);
        free_snapshot(d, snapshot);
    }
    const h2_pal_result_t result = d->report_result;
    diag_unlock(d, d->report_lock);
    return result;
}

h2_pal_result_t h2_mem_arena_diagnostics_flush(
    h2_mem_arena_diagnostics_t *d) {
    if (d == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    diag_lock(d, d->report_lock);
    if (d->reporter != NULL) {
        diag_snapshot_t *marker = NULL;
        h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_send(
            d->config.queue, d->reports, &marker, H2_PAL_QUEUE_WAIT_FOREVER);
        if (rc == H2_PAL_OK)
            rc = h2_pal_semaphore_take(d->config.sync, d->flush_done,
                                        H2_PAL_SYNC_WAIT_FOREVER);
        if (rc != H2_PAL_OK && d->report_result == H2_PAL_OK)
            d->report_result = rc;
    }
    const h2_pal_result_t result = d->report_result;
    diag_unlock(d, d->report_lock);
    return result;
}

h2_pal_result_t h2_mem_arena_diagnostics_poll(
    h2_mem_arena_diagnostics_t *d) {
    if (d == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (!d->config.trace || d->config.time == NULL)
        return H2_PAL_OK;
    uint64_t now_ms = 0u;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(d->config.time, &now_ms);
    if (rc != H2_PAL_OK)
        return rc;
    if (now_ms - d->last_poll_ms < 1000u)
        return H2_PAL_OK;
    d->last_poll_ms = now_ms;
    diag_lock(d, d->lock);
    const bool changed = d->peak.live >= d->polled_peak + 32u * 1024u;
    if (changed)
        d->polled_peak = d->peak.live;
    diag_unlock(d, d->lock);
    return changed ? h2_mem_arena_diagnostics_report(d, "peak_sample")
                   : H2_PAL_OK;
}

h2_pal_result_t h2_mem_arena_diagnostics_destroy(
    h2_mem_arena_diagnostics_t *d) {
    if (d == NULL)
        return H2_PAL_OK;
    diag_lock(d, d->lock);
    const bool live = d->live != 0u;
    diag_unlock(d, d->lock);
    if (live)
        return H2_PAL_ERR_INVALID_STATE;
    (void)h2_mem_arena_diagnostics_report(d, "shutdown");
    h2_pal_result_t rc = h2_mem_arena_diagnostics_flush(d);
    if (d->reporter != NULL) {
        const h2_pal_result_t close_rc =
            (h2_pal_result_t)h2_pal_queue_close(d->config.queue, d->reports);
        if (close_rc != H2_PAL_OK)
            return close_rc;
        const h2_pal_result_t join_rc =
            h2_pal_task_join(d->config.task, d->reporter);
        if (join_rc != H2_PAL_OK)
            return join_rc;
        d->reporter = NULL;
    }
    summarize(d);
    if (d->census != NULL) {
        const h2_pal_result_t census_rc = h2_mem_arena_census_destroy(d->census);
        if (census_rc != H2_PAL_OK)
            return census_rc;
        d->census = NULL;
    }
    const h2_pal_result_t arena_rc = h2_mem_arena_destroy(d->arena);
    if (arena_rc != H2_PAL_OK)
        return arena_rc;
    d->arena = NULL;
    release_resources(d);
    return rc;
}
