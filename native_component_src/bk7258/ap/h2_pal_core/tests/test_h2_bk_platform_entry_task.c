#include "h2_bk_platform_entry_task.h"
#include "h2_bk_platform_core.h"
#include <os/os.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static struct {
  h2_bk_task_policy_t policy;
  h2_pal_result_t reserve_result;
  int reserves, allocated, freed, context_psram, core, creates;
  int fail_alloc, fail_create, entries, cleanups, deletes, stack_psram;
  uint8_t priority;
  uint32_t stack;
  const char* sdk_name;
  beken_thread_function_t worker;
  beken_thread_arg_t worker_arg;
} s;

h2_pal_result_t h2_bk_platform_task_reserve_policy(const char* name,
                                                   h2_bk_task_policy_t* out) {
  assert(strcmp(name, "entry/test") == 0 && out != NULL);
  ++s.reserves;
  *out = s.policy;
  return s.reserve_result;
}
void* os_malloc(size_t size) {
  if (s.fail_alloc) return NULL;
  ++s.allocated;
  return malloc(size);
}
void* psram_malloc(size_t size) {
  s.context_psram = 1;
  return os_malloc(size);
}
void os_free(void* ptr) {
  assert(ptr != NULL && s.freed == 0);
  ++s.freed;
  free(ptr);
}
void rtos_delete_thread(beken_thread_t* thread) {
  assert(thread == NULL && s.entries == 1 && s.freed == 1 && s.deletes == 0);
  ++s.deletes;
}
static int create(beken_thread_t* thread, uint8_t priority, const char* name,
                  beken_thread_function_t fn, uint32_t stack, void* arg,
                  int core, int psram) {
  assert(thread != NULL && fn != NULL && arg != NULL && s.freed == 0);
  ++s.creates;
  s.priority = priority;
  s.stack = stack;
  s.sdk_name = name;
  s.worker = fn;
  s.worker_arg = arg;
  s.core = core;
  s.stack_psram = psram;
  *thread = &s;
  return s.fail_create ? kGeneralErr : kNoErr;
}
#define DEFINE_CREATE(name, core, psram)                                     \
  int name(beken_thread_t* thread, uint8_t priority, const char* task_name,  \
           beken_thread_function_t fn, uint32_t stack, void* arg) {          \
    return create(thread, priority, task_name, fn, stack, arg, core, psram); \
  }
DEFINE_CREATE(rtos_core0_create_thread, 0, 0)
DEFINE_CREATE(rtos_core1_create_thread, 1, 0)
DEFINE_CREATE(rtos_core0_create_psram_thread, 0, 1)
DEFINE_CREATE(rtos_core1_create_psram_thread, 1, 1)
DEFINE_CREATE(rtos_create_thread, -1, 0)
DEFINE_CREATE(rtos_create_psram_thread, -1, 1)
#undef DEFINE_CREATE

static void entry(void* user) {
  assert(user == &s.entries && s.freed == 1 && s.cleanups == 0 &&
         s.deletes == 0);
  ++s.entries;
}
static void cleanup(void* user) {
  assert(user == &s.cleanups && s.entries == 1 && s.deletes == 0);
  ++s.cleanups;
}
static int start(h2_bk_platform_entry_task_fn after_entry) {
  return h2_bk_platform_start_entry_task("entry/test", entry, &s.entries,
                                         after_entry, &s.cleanups);
}
static void reset(void) {
  memset(&s, 0, sizeof(s));
  s.policy = (h2_bk_task_policy_t){
      .priority = 6u,
      .core = 0u,
      .min_stack_size = 24576u,
      .stack_region = H2_BK_TASK_STACK_DEFAULT,
  };
}
int main(void) {
  reset();
  assert(h2_bk_platform_start_entry_task(NULL, entry, NULL, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_platform_start_entry_task("", entry, NULL, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_platform_start_entry_task("entry/test", NULL, NULL, NULL,
                                         NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(s.reserves == 0 && s.allocated == 0 && s.creates == 0);
  const int errors[] = {H2_PAL_ERR_INVALID_STATE, H2_PAL_ERR_NOT_FOUND,
                        H2_PAL_ERR_TASK};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    reset();
    s.reserve_result = errors[i];
    assert(start(cleanup) == errors[i]);
    assert(s.allocated == 0 && s.creates == 0);
  }
  for (int region = 0; region < 2; ++region) {
    reset();
    s.policy.stack_region = (h2_bk_task_stack_region_t)region;
    s.fail_alloc = 1;
    assert(start(cleanup) == H2_PAL_ERR_NO_MEMORY);
    assert(s.allocated == 0 && s.creates == 0 && s.freed == 0);
    s.fail_alloc = 0;
    s.fail_create = 1;
    assert(start(cleanup) == H2_PAL_ERR_TASK);
    assert(s.allocated == 1 && s.creates == 1 && s.freed == 1);
    assert(s.entries == 0 && s.cleanups == 0 && s.deletes == 0);
    const uint32_t cores[] = {0u, 1u, H2_BK_TASK_CORE_ANY};
    const uint32_t sizes[] = {24576u, 49152u, 65536u};
    for (size_t i = 0; i < sizeof(cores) / sizeof(cores[0]); ++i) {
#if !CONFIG_SOC_SMP
      if (cores[i] == 1u) continue;
#endif
      for (size_t j = 0; j < sizeof(sizes) / sizeof(sizes[0]); ++j) {
        reset();
        s.policy.core = cores[i];
        s.policy.priority = i == 0u ? 7u : 6u;
        s.policy.min_stack_size = sizes[j];
        s.policy.stack_region = (h2_bk_task_stack_region_t)region;
        if (j == 1u) s.policy.sdk_name = "native/renamed";
        assert(start(j == 2u ? NULL : cleanup) == H2_PAL_OK);
        assert(s.allocated == 1 && s.creates == 1 && s.freed == 0);
        assert(s.stack == sizes[j] && s.context_psram == region);
        assert(s.stack_psram == region && s.priority == (i == 0u ? 7u : 6u));
#if CONFIG_SOC_SMP
        assert(s.core ==
               (cores[i] == H2_BK_TASK_CORE_ANY ? -1 : (int)cores[i]));
#else
        assert(s.core == -1);
#endif
        assert(strcmp(s.sdk_name, j == 1u ? "native/renamed" : "entry/test") ==
               0);
        s.worker(s.worker_arg);
        assert(s.freed == 1 && s.entries == 1 && s.deletes == 1);
        assert(s.cleanups == (j == 2u ? 0 : 1));
      }
    }
  }
  reset();
  s.policy.min_stack_size = 16385u;
  s.policy.sdk_name = "";
  assert(start(cleanup) == H2_PAL_OK);
  assert(s.stack == 16388u && strcmp(s.sdk_name, "entry/test") == 0);
  s.worker(s.worker_arg);
  reset();
  s.policy.min_stack_size = 1u;
  assert(start(cleanup) == H2_PAL_OK && s.stack == 4096u);
  s.worker(s.worker_arg);
  return 0;
}
