#include "h2_pal_json_conformance.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdlib.h>

typedef struct allocation_counter {
    size_t allocations;
    size_t frees;
    size_t live_allocations;
} allocation_counter_t;

typedef struct allocation_header {
    allocation_counter_t *owner;
} allocation_header_t;

static void *test_alloc(void *user, size_t size) {
    allocation_counter_t *counter = user;
    ++counter->allocations;
    allocation_header_t *header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->owner = counter;
    ++counter->live_allocations;
    return header + 1;
}

static void *test_realloc(void *user, void *pointer, size_t size) {
    allocation_counter_t *counter = user;
    ++counter->allocations;
    if (pointer == NULL) {
        return test_alloc(user, size);
    }
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(header->owner == counter);
    header = realloc(header, sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->owner = counter;
    return header + 1;
}

static void test_free(void *user, void *pointer) {
    allocation_counter_t *counter = user;
    if (pointer == NULL) {
        return;
    }
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(header->owner == counter);
    assert(counter->live_allocations > 0u);
    --counter->live_allocations;
    ++counter->frees;
    free(header);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static h2_pal_result_t create_provider(
    const h2_pal_mem_api_t *mem,
    void **out_provider,
    const h2_pal_json_api_t **out_api) {
    if (out_provider == NULL || out_api == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_provider = NULL;
    *out_api = NULL;
    h2_yyjson_json_t *provider = NULL;
    const h2_pal_result_t result = h2_yyjson_json_create(mem, &provider);
    if (result != H2_PAL_OK) {
        return result;
    }
    *out_provider = provider;
    *out_api = h2_yyjson_json_api(provider);
    return H2_PAL_OK;
}

static h2_pal_result_t destroy_provider(void **provider) {
    if (provider == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_yyjson_json_t *typed_provider = *provider;
    const h2_pal_result_t result = h2_yyjson_json_destroy(&typed_provider);
    *provider = typed_provider;
    return result;
}

static const h2_pal_json_test_provider_t test_provider = {
    .name = "yyjson",
    .create = create_provider,
    .destroy = destroy_provider,
};

static void test_independent_instances(void) {
    allocation_counter_t first_counter = {0};
    allocation_counter_t second_counter = {0};
    const h2_pal_mem_api_t first_mem = {
        .user = &first_counter,
        .vtable = &test_mem_vtable,
    };
    const h2_pal_mem_api_t second_mem = {
        .user = &second_counter,
        .vtable = &test_mem_vtable,
    };
    h2_yyjson_json_t *first = NULL;
    h2_yyjson_json_t *second = NULL;
    assert(h2_yyjson_json_create(&first_mem, &first) == H2_PAL_OK);
    assert(h2_yyjson_json_create(&second_mem, &second) == H2_PAL_OK);
    assert(first != second);

    static const uint8_t input[] = {'[', '1', ']'};
    h2_pal_json_document_t *first_document = NULL;
    h2_pal_json_document_t *second_document = NULL;
    assert(h2_pal_json_document_parse(
               h2_yyjson_json_api(first), input, sizeof(input), NULL,
               &first_document) == H2_PAL_OK);
    assert(h2_pal_json_document_parse(
               h2_yyjson_json_api(second), input, sizeof(input), NULL,
               &second_document) == H2_PAL_OK);
    assert(first_counter.allocations > 0u);
    assert(second_counter.allocations > 0u);
    assert(h2_pal_json_document_destroy(
               h2_yyjson_json_api(first), &first_document) == H2_PAL_OK);
    assert(h2_pal_json_document_destroy(
               h2_yyjson_json_api(second), &second_document) == H2_PAL_OK);
    assert(h2_yyjson_json_destroy(&first) == H2_PAL_OK);
    assert(h2_yyjson_json_destroy(&second) == H2_PAL_OK);
    assert(first_counter.live_allocations == 0u);
    assert(second_counter.live_allocations == 0u);
    assert(first_counter.frees > 0u);
    assert(second_counter.frees > 0u);
    assert(h2_yyjson_json_api(NULL) == h2_pal_unsupported_json_api());
}

int main(void) {
    h2_pal_json_conformance_result_t result;
    h2_pal_json_run_conformance(&test_provider, &result);
    assert(result.peak_live_bytes > 0u);
    test_independent_instances();
    return 0;
}
