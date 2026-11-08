#include "h2_bk7258_board_task_test_sdk.h"

#include <assert.h>
#include <string.h>

static int allocated, freed, psram, core, create_calls, fail_alloc, fail_create;
static int returned, deinitialized, deleted;
static uint8_t created_priority;
static uint32_t created_stack;
static int created_psram;
static beken_thread_function_t worker;
static beken_thread_arg_t worker_arg;

void* os_malloc(size_t size) {
  if (fail_alloc) return NULL;
  ++allocated;
  return malloc(size);
}
void* psram_malloc(size_t size) {
  psram = 1;
  return os_malloc(size);
}
void os_free(void* pointer) {
  assert(pointer != NULL);
  ++freed;
  free(pointer);
}
h2_pal_result_t h2_bk7258_board_runtime_deinit(void) {
  assert(returned == 1);
  ++deinitialized;
  return H2_PAL_OK;
}
void rtos_delete_thread(beken_thread_t* thread) {
  assert(thread == NULL && deinitialized == 1);
  ++deleted;
}
static int create(beken_thread_t* thread, uint8_t priority, const char* name,
                  beken_thread_function_t function, uint32_t stack_size,
                  beken_thread_arg_t arg, int created_core, int stack_psram) {
  assert(thread != NULL && name != NULL && function != NULL && arg != NULL);
  created_psram = stack_psram;
  ++create_calls;
  created_priority = priority;
  created_stack = stack_size;
  core = created_core;
  worker = function;
  worker_arg = arg;
  *thread = (beken_thread_t)&worker;
  return fail_create ? -1 : kNoErr;
}
#define DEFINE_CREATE(name, created_core, stack_psram)                      \
  int name(beken_thread_t* thread, uint8_t priority, const char* task_name, \
           beken_thread_function_t function, uint32_t stack_size,           \
           beken_thread_arg_t arg) {                                        \
    return create(thread, priority, task_name, function, stack_size, arg,   \
                  created_core, stack_psram);                               \
  }
DEFINE_CREATE(rtos_core0_create_thread, 0, 0)
DEFINE_CREATE(rtos_core1_create_thread, 1, 0)
DEFINE_CREATE(rtos_core0_create_psram_thread, 0, 1)
DEFINE_CREATE(rtos_core1_create_psram_thread, 1, 1)
DEFINE_CREATE(rtos_create_thread, -1, 0)
DEFINE_CREATE(rtos_create_psram_thread, -1, 1)
#undef DEFINE_CREATE

static void entry(void* user) {
  assert(user == &returned && freed == 1);
  ++returned;
}
static void reset(void) {
  allocated = freed = psram = create_calls = fail_alloc = fail_create = 0;
  returned = deinitialized = deleted = 0;
  worker = NULL;
  worker_arg = NULL;
}
static void run(const char* name, uint32_t size, int stack_psram,
                int expected_core, uint8_t priority) {
  reset();
  assert(h2_bk7258_board_start_entry_task(name, entry, &returned) == H2_PAL_OK);
  assert(allocated == 1 && create_calls == 1 && freed == 0);
  assert(created_stack == size && psram == stack_psram);
#if CONFIG_SOC_SMP && CONFIG_CPU_CNT > 1
  assert(core == expected_core);
  assert(created_psram == stack_psram);
#else
  (void)expected_core;
  assert(core == -1);
  assert(created_psram == (strcmp(name, "bk/h106-e2e") == 0));
#endif
  assert(created_priority == priority);
  worker(worker_arg);
  assert(freed == 1 && returned == 1 && deinitialized == 1 && deleted == 1);
}
int main(void) {
  run("bk/h106-e2e", 64u * 1024u, 1, 0, BEKEN_APPLICATION_PRIORITY);
  run("bk/h106", 24u * 1024u, 1, 1, 6u);
  run("bk/h2loader", 48u * 1024u, 0, 0, 6u);
  run("bk/display", 24u * 1024u, 0, 1, 6u);
  run("bk/other", 24u * 1024u, 0, 0, BEKEN_APPLICATION_PRIORITY);
  reset();
  assert(h2_bk7258_board_start_entry_task(NULL, entry, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk7258_board_start_entry_task("", entry, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk7258_board_start_entry_task("bk/h106-e2e", NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(allocated == 0 && create_calls == 0);
  fail_alloc = 1;
  assert(h2_bk7258_board_start_entry_task("bk/h106-e2e", entry, &returned) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(allocated == 0 && create_calls == 0 && freed == 0);
  reset();
  fail_create = 1;
  assert(h2_bk7258_board_start_entry_task("bk/h106-e2e", entry, &returned) ==
         H2_PAL_ERR_TASK);
  assert(allocated == 1 && create_calls == 1 && freed == 1 && returned == 0);
  return 0;
}
