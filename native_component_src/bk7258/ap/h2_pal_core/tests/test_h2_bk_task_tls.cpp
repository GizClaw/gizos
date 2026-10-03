#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
#include "h2_bk_task_tls.h"
#include "h2_bk_task_tls_hooks.h"
#include "h2_bk_socket_errno_reader.h"
void test_socket_failure(int error);
}
extern "C" { int test_legacy_errno; }

struct Task { void *slots[3] = {}; };
static Task *current;
static bool interrupt_context;
static unsigned allocations, legacy_cleanups, fallback_reads;
static int fallback_errno;
static unsigned char fallback_tls[64];

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle() { return current; }
extern "C" bool rtos_is_in_interrupt_context() { return interrupt_context; }
extern "C" void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t task, int slot) {
  return static_cast<Task *>(task)->slots[slot];
}
extern "C" void vTaskSetThreadLocalStoragePointer(TaskHandle_t task, int slot, void *value) {
  static_cast<Task *>(task)->slots[slot] = value;
}
extern "C" void *os_malloc(size_t size) {
  // Simulate an allocator that accesses errno during lazy TLS initialization.
  (void)__wrap___errno();
  void *value = std::malloc(size);
  assert(value != nullptr);
  ++allocations;
  return value;
}
extern "C" void os_free(void *value) { --allocations; std::free(value); }
extern "C" int *__real___errno() { ++fallback_reads; return &fallback_errno; }
extern "C" void *__real___emutls_get_address(h2_bk_emutls_control_t *) { return fallback_tls; }
extern "C" void test_legacy_port_cleanup(void *) { ++legacy_cleanups; }

int main() {
  Task first, second;
  first.slots[0] = reinterpret_cast<void *>(11);
  first.slots[1] = reinterpret_cast<void *>(22);
  const std::uint32_t initial = 7;
  h2_bk_emutls_control_t control = {sizeof(initial), 64, 0, &initial};
  h2_bk_emutls_control_t zero = {13, 32, 0, nullptr};

  // Two virtual tasks on one host thread: host TLS cannot make this pass.
  current = &first;
  int *first_errno = __wrap___errno();
  test_socket_failure(11); // vendor lwIP failed nonblocking recv: EAGAIN
  assert(errno == 11 && test_legacy_errno == 11);
  auto *first_value = static_cast<std::uint32_t *>(__wrap___emutls_get_address(&control));
  assert(*first_value == initial && reinterpret_cast<std::uintptr_t>(first_value) % 64 == 0);
  *first_value = 100;
  auto *zeros = static_cast<unsigned char *>(__wrap___emutls_get_address(&zero));
  assert(reinterpret_cast<std::uintptr_t>(zeros) % 32 == 0);
  for (unsigned i = 0; i < 13; ++i) assert(zeros[i] == 0);

  current = &second;
  int *second_errno = __wrap___errno();
  test_socket_failure(88); // another task fails on a closed socket: ENOTSOCK
  assert(errno == 88 && test_legacy_errno == 88);
  test_socket_failure(0);
  assert(errno == 88 && test_legacy_errno == 88); // Preserve no-clear semantics.
  auto *second_value = static_cast<std::uint32_t *>(__wrap___emutls_get_address(&control));
  assert(first_errno != second_errno && first_value != second_value && *second_value == initial);
  current = &first;
  assert(__wrap___errno() == first_errno && *first_errno == 11);
  assert(errno == 11 && test_legacy_errno == 88);
  assert(__wrap___emutls_get_address(&control) == first_value && *first_value == 100);
  assert(control.location == 0); // Never reuse libgcc's one global location.
  assert(first.slots[0] == reinterpret_cast<void *>(11) && first.slots[1] == reinterpret_cast<void *>(22));
  assert(fallback_reads != 0); // Allocator errno recursion used the safe fallback.

  interrupt_context = true;
  assert(__wrap___errno() == &fallback_errno);
  interrupt_context = false;
  current = nullptr;
  assert(__wrap___errno() == &fallback_errno);
  assert(__wrap___emutls_get_address(&control) == fallback_tls);

  // Kernel cleanup occurs after execution ends; preserve the existing hook.
  h2_bk_task_tls_port_cleanup(&first);
  assert(first.slots[2] == nullptr && legacy_cleanups == 1);
  current = &first; // Reuse the same TCB address with fresh values.
  assert(*__wrap___errno() == 0);
  assert(*static_cast<std::uint32_t *>(__wrap___emutls_get_address(&control)) == initial);
  h2_bk_task_tls_port_cleanup(&first);
  h2_bk_task_tls_port_cleanup(&second);
  assert(allocations == 0 && legacy_cleanups == 3);
  std::puts("task errno/emutls isolation, alignment, template, reuse and cleanup PASS");
}
