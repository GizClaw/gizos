#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "tools/bazel/tests/task_stack_accounting/task_policy/h2_target_stack_accounting.inc"

int main(void) {
  assert(h2_target_task_stack_bytes("worker", 4096) == 65536);
  assert(h2_target_task_stack_bytes("worker", 131072) == 131072);
  assert(h2_target_task_stack_bytes("internal", 131072) == 0);
  assert(h2_target_task_stack_bytes("group/child", 0) == 8192);
  assert(h2_target_task_stack_bytes("group/child", 32768) == 32768);
  assert(h2_target_task_stack_bytes("group/special", 0) == 0);
  assert(h2_target_task_stack_bytes("group/nested/child", 0) == 32768);
  assert(h2_target_task_stack_bytes("tiny", 0) == 4096);
  assert(h2_target_task_stack_bytes("dynamic", 0) == 16384);
  assert(h2_target_task_stack_bytes(NULL, 0) == 0);
  assert(h2_target_task_stack_bytes("", 0) == 0);
  return 0;
}
