#include "probe.h"

#include "h2_yyjson_json.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef union allocation_header {
    size_t size;
#if defined(_MSC_VER) && !defined(__clang__)
    /* MSVC's C headers omit max_align_t; cover its scalar alignments. */
    long double floating_alignment;
    long long integer_alignment;
    void *pointer_alignment;
#else
    max_align_t alignment;
#endif
} allocation_header_t;

typedef struct tracked_mem {
    const h2_pal_mem_api_t *base;
    size_t calls;
    size_t fail_at;
    size_t live;
    size_t bytes;
} tracked_mem_t;

static int fail_now(tracked_mem_t *state) {
    return state->calls++ == state->fail_at;
}

static void *tracked_alloc(void *user, size_t size) {
    tracked_mem_t *state = user;
    if (fail_now(state) || size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
    allocation_header_t *header = h2_pal_mem_alloc(state->base, sizeof(*header) + size);
    if (!header) return NULL;
    header->size = size;
    ++state->live;
    state->bytes += size;
    return header + 1;
}

static void tracked_free(void *user, void *pointer) {
    tracked_mem_t *state = user;
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    if (state->live && state->bytes >= header->size) {
        --state->live;
        state->bytes -= header->size;
    } else {
        state->live = SIZE_MAX;
    }
    h2_pal_mem_free(state->base, header);
}

static void *tracked_realloc(void *user, void *pointer, size_t size) {
    tracked_mem_t *state = user;
    if (!pointer) return tracked_alloc(user, size);
    if (!size) { tracked_free(user, pointer); return NULL; }
    if (fail_now(state) || size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
    allocation_header_t *old = (allocation_header_t *)pointer - 1;
    size_t old_size = old->size;
    allocation_header_t *header = h2_pal_mem_realloc(state->base, old, sizeof(*header) + size);
    if (!header) return NULL;
    header->size = size;
    state->bytes = state->bytes - old_size + size;
    return header + 1;
}

static const h2_pal_mem_vtable_t tracked_vtable = {
    .alloc = tracked_alloc,
    .realloc = tracked_realloc,
    .free = tracked_free,
};

static h2_pal_result_t check_live_output(const h2_pal_mem_api_t *base) {
    tracked_mem_t state = {.base = base, .fail_at = SIZE_MAX};
    h2_pal_mem_api_t mem = {.user = &state, .vtable = &tracked_vtable};
    h2_yyjson_json_t *provider = NULL;
    h2_pal_json_document_t *document = NULL;
    h2_pal_json_buffer_t buffer = {0};
    h2_pal_result_t rc = h2_yyjson_json_create(&mem, &provider);
    if (rc != H2_PAL_OK || !provider) goto done;
    const h2_pal_json_api_t *api = h2_yyjson_json_api(provider);
    if (h2_pal_json_document_parse(api, (const uint8_t *)"[]", 2, NULL, &document) != H2_PAL_OK ||
        h2_pal_json_document_serialize(api, document, &buffer) != H2_PAL_OK ||
        h2_yyjson_json_destroy(&provider) != H2_PAL_ERR_INVALID_STATE || !provider) {
        rc = H2_PAL_ERR_INVALID_STATE;
        goto done;
    }
    if (h2_pal_json_document_destroy(api, &document) != H2_PAL_OK ||
        h2_yyjson_json_destroy(&provider) != H2_PAL_ERR_INVALID_STATE || !provider ||
        h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK ||
        h2_yyjson_json_destroy(&provider) != H2_PAL_OK || provider) {
        rc = H2_PAL_ERR_INVALID_STATE;
        goto done;
    }
done:
    if (provider) {
        const h2_pal_json_api_t *cleanup_api = h2_yyjson_json_api(provider);
        if (buffer.data) (void)h2_pal_json_buffer_release(cleanup_api, &buffer);
        if (document) (void)h2_pal_json_document_destroy(cleanup_api, &document);
        if (h2_yyjson_json_destroy(&provider) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (state.live != 0 || state.bytes != 0 || provider) rc = H2_PAL_ERR_INVALID_STATE;
    return rc;
}

static h2_pal_result_t one_failure_trial(const h2_pal_mem_api_t *base,
                                        size_t fail_at, int mutation,
                                        int *saw_no_memory, int *saw_success) {
    tracked_mem_t state = {.base = base, .fail_at = fail_at};
    h2_pal_mem_api_t mem = {.user = &state, .vtable = &tracked_vtable};
    h2_yyjson_json_t *provider = NULL;
    h2_pal_json_document_t *document = NULL;
    h2_pal_json_value_t *root = NULL, *string = NULL;
    h2_pal_json_buffer_t buffer = {0};
    static const uint8_t input[] = "{\"a\":[1,2,3],\"b\":{\"c\":\"value\"}}";
    h2_pal_result_t rc = h2_yyjson_json_create(&mem, &provider);
    if (rc == H2_PAL_OK) {
        const h2_pal_json_api_t *api = h2_yyjson_json_api(provider);
        if (!mutation) {
            rc = h2_pal_json_document_parse(api, input, sizeof(input) - 1, NULL, &document);
        } else {
            rc = h2_pal_json_document_create(api, NULL, &document);
            if (rc == H2_PAL_OK) rc = h2_pal_json_value_create_object(api, document, &root);
            if (rc == H2_PAL_OK) rc = h2_pal_json_document_set_root(api, document, root);
            if (rc == H2_PAL_OK) rc = h2_pal_json_value_create_string(api, document, "value", 5, &string);
            if (rc == H2_PAL_OK) rc = h2_pal_json_object_set(api, root, "key", 3, string);
            if (rc == H2_PAL_OK) rc = h2_pal_json_document_serialize(api, document, &buffer);
        }
        if (buffer.data && h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
        if (document && h2_pal_json_document_destroy(api, &document) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
        if (h2_yyjson_json_destroy(&provider) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_ERR_NO_MEMORY) *saw_no_memory = 1;
    else if (rc == H2_PAL_OK) *saw_success = 1;
    else return H2_PAL_ERR_INVALID_STATE;
    if (provider || document || buffer.data || state.live || state.bytes) return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
}

h2_pal_result_t h2_json_yyjson_probe(const h2_pal_mem_api_t *mem,
                                     h2_pal_json_e2e_probe_t probe) {
    if (!mem || !mem->vtable || !mem->vtable->alloc || !mem->vtable->realloc || !mem->vtable->free)
        return H2_PAL_ERR_INVALID_ARG;
    if (probe == H2_PAL_JSON_E2E_PROBE_LIVE_OUTPUT) return check_live_output(mem);
    if (probe != H2_PAL_JSON_E2E_PROBE_ALLOCATOR_FAILURE) return H2_PAL_ERR_INVALID_ARG;
    for (int mutation = 0; mutation < 2; ++mutation) {
        int saw_no_memory = 0, saw_success = 0;
        for (size_t fail_at = 0; fail_at < 128; ++fail_at) {
            h2_pal_result_t rc = one_failure_trial(mem, fail_at, mutation, &saw_no_memory, &saw_success);
            if (rc != H2_PAL_OK) return rc;
        }
        if (!saw_no_memory || !saw_success) return H2_PAL_ERR_INVALID_STATE;
    }
    return H2_PAL_OK;
}
