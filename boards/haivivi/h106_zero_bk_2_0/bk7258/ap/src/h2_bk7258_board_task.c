#ifdef H2_BK7258_BOARD_TASK_HOST_TEST
#include "h2_bk7258_board_task_test_sdk.h"
#else
#include "h2_bk7258_board.h"
#include "os/os.h"
#include "os/mem.h"
#endif

#include <stdio.h>
#include <string.h>

#define H2_BK7258_ENTRY_TASK_STACK_SIZE (24u * 1024u)
/* The E2E runner nests product snapshots and synchronous fixture calls. */
#define H2_BK7258_H106_E2E_TASK_STACK_SIZE (64u * 1024u)

typedef struct h2_bk7258_board_entry_task_context {
  h2_bk7258_board_entry_task_fn entry;
  void* user;
} h2_bk7258_board_entry_task_context_t;

static void entry_task(beken_thread_arg_t raw) {
  h2_bk7258_board_entry_task_context_t* context =
      (h2_bk7258_board_entry_task_context_t*)raw;
  h2_bk7258_board_entry_task_fn entry = context->entry;
  void* user = context->user;
  os_free(context);
  entry(user);
  (void)h2_bk7258_board_runtime_deinit();
  rtos_delete_thread(NULL);
}

h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char* name, h2_bk7258_board_entry_task_fn entry, void* user) {
  uint8_t core = 0u;
  uint8_t priority = BEKEN_APPLICATION_PRIORITY;
  int fallback = 1;

  if (name == NULL || name[0] == '\0' || entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (strcmp(name, "bk/h106") == 0 || strcmp(name, "bk/audio-system") == 0 ||
      strcmp(name, "bk/display") == 0 || strcmp(name, "bk/lvgl-smoke") == 0 ||
      strcmp(name, "bk/mp4-small") == 0) {
    core = 1u;
    priority = 6u;
    fallback = 0;
  } else if (strcmp(name, "bk/h2loader") == 0) {
    priority = 6u;
    fallback = 0;
  }
  const int h106_e2e = strcmp(name, "bk/h106-e2e") == 0;
  const int use_psram = strcmp(name, "bk/h106") == 0 || h106_e2e;
  const uint32_t stack_size = h106_e2e ? H2_BK7258_H106_E2E_TASK_STACK_SIZE
                                       : H2_BK7258_ENTRY_TASK_STACK_SIZE;
  if (h106_e2e) fallback = 0;
  h2_bk7258_board_entry_task_context_t* context =
      use_psram ? psram_malloc(sizeof(*context)) : os_malloc(sizeof(*context));
  if (context == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  context->entry = entry;
  context->user = user;

  beken_thread_t thread = NULL;
  int result;
#if defined(CONFIG_SOC_SMP) && CONFIG_SOC_SMP && defined(CONFIG_CPU_CNT) && \
    CONFIG_CPU_CNT > 1
  if (core == 1u) {
    result = use_psram ? rtos_core1_create_psram_thread(
                             &thread, priority, name, entry_task, stack_size,
                             (beken_thread_arg_t)context)
                       : rtos_core1_create_thread(&thread, priority, name,
                                                  entry_task, stack_size,
                                                  (beken_thread_arg_t)context);
  } else {
    result = use_psram ? rtos_core0_create_psram_thread(
                             &thread, priority, name, entry_task, stack_size,
                             (beken_thread_arg_t)context)
                       : rtos_core0_create_thread(&thread, priority, name,
                                                  entry_task, stack_size,
                                                  (beken_thread_arg_t)context);
  }
#else
  result =
      h106_e2e
          ? rtos_create_psram_thread(&thread, priority, name, entry_task,
                                     stack_size, (beken_thread_arg_t)context)
          : rtos_create_thread(&thread, priority, name, entry_task, stack_size,
                               (beken_thread_arg_t)context);
#endif
  if (result != kNoErr) {
    os_free(context);
    return H2_PAL_ERR_TASK;
  }
  if (fallback) {
    printf(
        "H2_PAL_TASK_FALLBACK name=%s core=%u priority=%u "
        "stack=default size=%u\n",
        name, (unsigned int)core, (unsigned int)priority,
        (unsigned int)stack_size);
  }
  printf(
      "H2_PAL_TASK_READY name=%s core=%u priority=%u "
      "stack=%s size=%u\n",
      name, (unsigned int)core, (unsigned int)priority,
      use_psram ? "psram" : "default", (unsigned int)stack_size);
  return H2_PAL_OK;
}
