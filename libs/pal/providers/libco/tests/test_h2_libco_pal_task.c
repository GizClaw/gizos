#include "h2_libco_test_support.h"

typedef struct task_context {
    h2_libco_t *core;
    h2_libco_t *other;
    unsigned calls;
} task_context_t;

static void pal_entry(void *user) {
    task_context_t *context = user;
    assert(h2_libco_current_task(context->core) != NULL);
    assert(h2_libco_current_task(context->other) == NULL);
    h2_libco_resource_stats_t stats;
    assert(h2_libco_get_resource_stats(context->core, &stats) == H2_LIBCO_OK);
    assert(stats.live_tasks >= 1u && stats.task_stack_bytes != 0u);
    assert(h2_libco_get_resource_stats(context->other, &stats) == H2_LIBCO_ERR_INVALID_STATE);
    assert(stats.live_tasks == 0u && stats.task_stack_bytes == 0u);
    ++context->calls;
}

static int native_entry(void *user) {
    task_context_t *context = user;
    assert(h2_libco_current_task(context->core) != NULL);
    ++context->calls;
    return 91;
}

typedef struct joining_context {
    task_context_t context;
    const h2_pal_task_api_t *api;
} joining_context_t;

static void joining_entry(void *user) {
    joining_context_t *context = user;
    const h2_libco_task_t *identity = h2_libco_current_task(context->context.core);
    assert(identity != NULL);
    for (unsigned cycle = 0; cycle < 100u; ++cycle) {
        h2_pal_task_t *child = NULL;
        assert(h2_pal_task_start(context->api, NULL, pal_entry,
                                 &context->context, &child) == H2_PAL_OK);
        assert(h2_pal_task_join(context->api, child) == H2_PAL_OK);
        assert(h2_libco_current_task(context->context.core) == identity);
    }
}

int main(void) {
    h2_libco_test_env_t env = {0}, other_env = {0};
    h2_libco_t *core = h2_libco_test_create(&env);
    h2_libco_t *other = h2_libco_test_create(&other_env);
    const h2_pal_task_api_t *api = h2_libco_task_api(core);
    task_context_t context = {.core = core, .other = other};
    const size_t core_allocations = env.allocations;
    const size_t core_bytes = env.allocated_bytes;
    assert(h2_libco_current_task(core) == NULL);
    assert(h2_libco_current_task(NULL) == NULL);
    h2_libco_resource_stats_t stats;
    assert(h2_libco_get_resource_stats(NULL, &stats) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_get_resource_stats(core, NULL) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_get_resource_stats(core, &stats) == H2_LIBCO_OK);
    assert(stats.live_tasks == 0u && stats.task_stack_bytes == 0u);
    h2_pal_task_t *task = NULL;
    const h2_pal_task_options_t options = {
        .name = "not-retained",
        .min_stack_size = 1u,
    };
    assert(h2_pal_task_start(api, &options, pal_entry, &context, &task) ==
           H2_PAL_OK);
    assert(context.calls == 0u);
    assert(h2_pal_task_join(api, task) == H2_PAL_ERR_BUSY);
    assert(h2_libco_destroy(&core) == H2_LIBCO_ERR_BUSY);
    h2_libco_test_schedule(core, 1u);
    assert(context.calls == 1u);
    assert(h2_libco_get_resource_stats(core, &stats) == H2_LIBCO_OK);
    assert(stats.live_tasks == 1u && stats.task_stack_bytes >= options.min_stack_size);
    assert(h2_pal_task_join(api, task) == H2_PAL_OK);
    assert(h2_libco_get_resource_stats(core, &stats) == H2_LIBCO_OK);
    assert(stats.live_tasks == 0u && stats.task_stack_bytes == 0u);
    assert(env.allocations == core_allocations && env.allocated_bytes == core_bytes);

    /* Native clients still retain their rejected joined tombstone. Mixing
     * native and PAL tasks must not make PAL reclamation unlink that handle. */
    h2_libco_task_t *native = NULL;
    assert(h2_libco_task_start(core, NULL, native_entry, &context, &native) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    int native_result = 0;
    assert(h2_libco_task_join(core, native, &native_result) == H2_LIBCO_OK);
    assert(native_result == 91);
    assert(h2_libco_get_resource_stats(core, &stats) == H2_LIBCO_OK);
    assert(stats.live_tasks == 0u && stats.task_stack_bytes == 0u);
    const size_t tombstone_allocations = env.allocations;
    const size_t tombstone_bytes = env.allocated_bytes;
    assert(tombstone_allocations == core_allocations + 1u);
    assert(tombstone_bytes > core_bytes);
    for (unsigned cycle = 0; cycle < 100u; ++cycle) {
        task = NULL;
        assert(h2_pal_task_start(api, NULL, pal_entry, &context, &task) == H2_PAL_OK);
        h2_libco_test_schedule(core, 1u);
        assert(h2_pal_task_join(api, task) == H2_PAL_OK);
        assert(env.allocations == tombstone_allocations);
        assert(env.allocated_bytes == tombstone_bytes);
        assert(h2_libco_get_resource_stats(core, &stats) == H2_LIBCO_OK);
        assert(stats.live_tasks == 0u && stats.task_stack_bytes == 0u);
        assert(h2_libco_task_join(core, native, NULL) == H2_LIBCO_ERR_INVALID_STATE);
        assert(h2_libco_task_cancel(core, native) == H2_LIBCO_ERR_INVALID_STATE);
    }
    h2_pal_task_t *batch[8] = {0};
    for (size_t i = 0; i < 8u; ++i)
        assert(h2_pal_task_start(api, NULL, pal_entry, &context, &batch[i]) == H2_PAL_OK);
    h2_libco_test_schedule(core, 8u);
    /* Oldest-first join releases nodes in the middle/tail of the native list. */
    for (size_t i = 0; i < 8u; ++i)
        assert(h2_pal_task_join(api, batch[i]) == H2_PAL_OK);
    assert(env.allocations == tombstone_allocations && env.allocated_bytes == tombstone_bytes);

    joining_context_t joining = {.context = {.core = core, .other = other}, .api = api};
    task = NULL;
    assert(h2_pal_task_start(api, NULL, joining_entry, &joining, &task) == H2_PAL_OK);
    for (unsigned turns = 0; turns < 400u; ++turns) {
        size_t resumed;
        assert(h2_libco_schedule(core, 64u, &resumed) == H2_LIBCO_OK);
        if (resumed == 0u) break;
    }
    assert(joining.context.calls == 100u);
    assert(h2_pal_task_join(api, task) == H2_PAL_OK);
    assert(env.allocations == tombstone_allocations && env.allocated_bytes == tombstone_bytes);
    assert(h2_libco_task_join(core, native, NULL) == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_current_task(core) == NULL);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&other) == H2_LIBCO_OK);
    assert(core == NULL && env.allocations == 0u && env.allocated_bytes == 0u);
    assert(other_env.allocations == 0u && other_env.allocated_bytes == 0u);
    return 0;
}
