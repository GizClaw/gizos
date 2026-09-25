#ifndef H2_MEM_ARENA_CENSUS_H
#define H2_MEM_ARENA_CENSUS_H

#include "h2_mem_arena.h"
#include "h2/pal/os/h2_pal_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_mem_arena_census h2_mem_arena_census_t;

/** Optional provider-owned caller capture. Outputs are zero on failure;
 * the census then attributes the request to site zero. Implementations may
 * use platform stack facilities but must not allocate or reenter the census. */
typedef h2_pal_result_t (*h2_mem_arena_census_capture_site_fn)(
    void *user, uintptr_t *out_caller, uintptr_t *out_outer);

typedef struct h2_mem_arena_census_tag_config {
    /** Borrowed name, unique within config->tags and valid until destroy. */
    const char *name;
    /** Attribute this tag's allocations to best-effort caller addresses. */
    bool track_sites;
} h2_mem_arena_census_tag_config_t;

typedef struct h2_mem_arena_census_config {
    /** Borrowed arena; it must outlive the census and all its allocations. */
    h2_mem_arena_t *arena;
    /** Borrowed Memory PAL backed by arena; NULL selects h2_mem_arena_mem.
     * An adapter may log failed allocations, but must preserve arena pointers. */
    const h2_pal_mem_api_t *inner;
    /** Borrowed allocator outside the measured arena, used only at create and
     * destroy for bounded metadata and the two priority-inheriting mutexes. */
    const h2_pal_mem_api_t *metadata;
    const h2_pal_sync_api_t *sync;
    /** Borrowed tag descriptors. At least one tag is required; the caller
     * chooses which index means "other" and maps unknown owners to it. */
    const h2_mem_arena_census_tag_config_t *tags;
    size_t tag_count;
    h2_mem_arena_census_capture_site_fn capture_site;
    void *capture_site_user;
    /** Fixed capacities allocated at create. Site slot zero collects
     * unattributed calls; site_capacity must be at least two. */
    size_t block_capacity;
    size_t site_capacity;
    /** Bounded linear probe for both tables; 1..min(capacities - 1). */
    size_t probe_limit;
} h2_mem_arena_census_config_t;

typedef struct h2_mem_arena_census_counts {
    size_t live_bytes;
    size_t blocks;
    size_t peak_bytes;
    size_t small_live_bytes;
    size_t small_blocks;
    size_t small_peak_bytes;
} h2_mem_arena_census_counts_t;

typedef struct h2_mem_arena_census_tag_snapshot {
    const char *name; /* Borrowed from config->tags until destroy. */
    h2_mem_arena_census_counts_t counts;
} h2_mem_arena_census_tag_snapshot_t;

typedef struct h2_mem_arena_census_site_snapshot {
    uintptr_t caller;
    uintptr_t outer;
    size_t tag_index;
    h2_mem_arena_census_counts_t counts;
    bool used;
} h2_mem_arena_census_site_snapshot_t;

typedef struct h2_mem_arena_census_snapshot {
    /** Arrays are borrowed only during the visitor call. Index zero in sites
     * aggregates site/table overflows and unavailable call addresses. */
    const h2_mem_arena_census_tag_snapshot_t *tags;
    size_t tag_count;
    const h2_mem_arena_census_site_snapshot_t *sites;
    size_t site_capacity;
    size_t site_overflow;
    size_t block_overflow;
    /** Live blocks whose owner could not fit the bounded block table. Their
     * aggregate remains exact even when another tag view frees/reallocs one. */
    h2_mem_arena_census_counts_t unattributed_overflow;
    size_t allocation_failures;
    /** Provider-owned tables and handle; excludes opaque Sync PAL mutexes. */
    size_t metadata_bytes;
} h2_mem_arena_census_snapshot_t;

typedef void (*h2_mem_arena_census_visit_fn)(
    void *user, const h2_mem_arena_census_snapshot_t *snapshot);

/** @brief Create an optional Memory PAL census around one arena.
 * Allocates metadata only during create; no allocation, formatting or logging
 * is added to the measured allocation path. Requires alloc/free metadata and
 * priority-inheriting sync mutexes; callers stop/join all users before destroy.
 * Returns INVALID_ARG for malformed config, UNSUPPORTED for absent sync,
 * NO_MEMORY for metadata exhaustion, or the mutex creation error; *out is
 * NULL on every error.
 */
h2_pal_result_t h2_mem_arena_census_create(
    const h2_mem_arena_census_config_t *config,
    h2_mem_arena_census_t **out_census);

/** @brief Borrow one tag's Memory PAL until destroy; invalid index returns
 * NULL. Any view may free/realloc a block, including on table overflow.
 * The underlying arena controls alignment, zero-size and realloc failure.
 */
const h2_pal_mem_api_t *h2_mem_arena_census_tag_mem(
    h2_mem_arena_census_t *census, size_t tag_index);

/** @brief Copy counters under a short allocation mutex and visit outside it.
 * A separate snapshot mutex serializes visitors. The callback may use a tag
 * allocator but must not recursively request a snapshot or destroy the census.
 * On block-table overflow, per-tag attribution degrades into
 * unattributed_overflow; aggregate counts and business allocation semantics
 * remain intact even when a different tag view frees/reallocs that block.
 * This function does not format output, rank sites or schedule sampling.
 */
h2_pal_result_t h2_mem_arena_census_snapshot(
    h2_mem_arena_census_t *census, h2_mem_arena_census_visit_fn visit,
    void *user);

/** @brief Refuse live blocks, including unattributed overflow blocks, with
 * INVALID_STATE, retaining the census.
 * NULL succeeds. A successful destroy invalidates the borrowed tag views,
 * but never destroys the arena or other borrowed dependencies.
 */
h2_pal_result_t h2_mem_arena_census_destroy(h2_mem_arena_census_t *census);

#ifdef __cplusplus
}
#endif

#endif
