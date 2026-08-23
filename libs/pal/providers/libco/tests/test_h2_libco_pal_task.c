#include "h2_libco_test_support.h"

typedef struct task_context {
    unsigned calls;
} task_context_t;

static void pal_entry(void *user) {
    ++((task_context_t *)user)->calls;
}

int main(void) {
    h2_libco_test_env_t env = {0};
    h2_libco_t *core = h2_libco_test_create(&env);
    const h2_pal_task_api_t *api = h2_libco_task_api(core);
    task_context_t context = {0};
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
    assert(h2_pal_task_join(api, task) == H2_PAL_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(core == NULL && env.allocations == 0u);
    return 0;
}
