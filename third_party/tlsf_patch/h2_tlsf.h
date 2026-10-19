#ifndef H2_TLSF_H
#define H2_TLSF_H

/* ESP-IDF's heap component links its own TLSF with the same public names.
 * Prefix every external symbol of the vendored copy so both can coexist. */
#define tlsf_create h2_tlsf_create
#define tlsf_create_with_pool h2_tlsf_create_with_pool
#define tlsf_destroy h2_tlsf_destroy
#define tlsf_get_pool h2_tlsf_get_pool
#define tlsf_add_pool h2_tlsf_add_pool
#define tlsf_remove_pool h2_tlsf_remove_pool
#define tlsf_malloc h2_tlsf_malloc
#define tlsf_memalign h2_tlsf_memalign
#define tlsf_realloc h2_tlsf_realloc
#define tlsf_free h2_tlsf_free
#define tlsf_block_size h2_tlsf_block_size
#define tlsf_size h2_tlsf_size
#define tlsf_align_size h2_tlsf_align_size
#define tlsf_block_size_min h2_tlsf_block_size_min
#define tlsf_block_size_max h2_tlsf_block_size_max
#define tlsf_pool_overhead h2_tlsf_pool_overhead
#define tlsf_alloc_overhead h2_tlsf_alloc_overhead
#define tlsf_walk_pool h2_tlsf_walk_pool
#define tlsf_check h2_tlsf_check
#define tlsf_check_pool h2_tlsf_check_pool

#include "tlsf.h"

#endif
