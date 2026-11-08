#include "h2_bk_platform_entry_task.h"

#include "h2_bk_task_create_internal.h"

#include <os/mem.h>
#include <os/os.h>
#include "FreeRTOS.h"

typedef struct h2_bk_entry_context {
  h2_bk_platform_entry_task_fn entry;
  void* user;
  h2_bk_platform_entry_task_fn after_entry;
  void* after_entry_user;
} h2_bk_entry_context_t;

static void entry_task(beken_thread_arg_t raw) {
  const h2_bk_entry_context_t context = *(h2_bk_entry_context_t*)raw;
  os_free(raw);
  context.entry(context.user);
  if (context.after_entry != NULL) {
    context.after_entry(context.after_entry_user);
  }
  (void)rtos_delete_thread(NULL);
}

h2_pal_result_t h2_bk_platform_start_entry_task(
    const char* name, h2_bk_platform_entry_task_fn entry, void* user,
    h2_bk_platform_entry_task_fn after_entry, void* after_entry_user) {
  if (name == NULL || name[0] == '\0' || entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_bk_task_policy_t policy = {0};
  h2_pal_result_t rc = h2_bk_platform_task_reserve_policy(name, &policy);
  if (rc != H2_PAL_OK) return rc;
  h2_bk_entry_context_t* context = policy.stack_region == H2_BK_TASK_STACK_PSRAM
                                       ? psram_malloc(sizeof(*context))
                                       : os_malloc(sizeof(*context));
  if (context == NULL) return H2_PAL_ERR_NO_MEMORY;
  *context = (h2_bk_entry_context_t){
      .entry = entry,
      .user = user,
      .after_entry = after_entry,
      .after_entry_user = after_entry_user,
  };
  uint32_t stack_size =
      policy.min_stack_size < 4096u ? 4096u : policy.min_stack_size;
  stack_size = (stack_size + sizeof(StackType_t) - 1u) / sizeof(StackType_t) *
               sizeof(StackType_t);
  const char* sdk_name = policy.sdk_name != NULL && policy.sdk_name[0] != '\0'
                             ? policy.sdk_name
                             : name;
  beken_thread_t thread = NULL;
  if (h2_bk_task_create(&thread, &policy, sdk_name, entry_task, stack_size,
                        (beken_thread_arg_t)context) != kNoErr) {
    os_free(context);
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}
