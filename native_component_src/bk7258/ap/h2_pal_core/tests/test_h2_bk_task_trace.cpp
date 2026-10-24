#include "h2_bk_task_trace.h"
#include "h2_bk_task_trace_internal.h"
#include "h2_bk_task_trace_test_sdk.h"

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __clang__
#define TEST_CAPABILITY __attribute__((capability("mutex")))
#define TEST_ACQUIRE(...) __attribute__((acquire_capability(__VA_ARGS__)))
#define TEST_RELEASE(...) __attribute__((release_capability(__VA_ARGS__)))
#define TEST_NATIVE_LOCK __attribute__((no_thread_safety_analysis))
#else
#define TEST_CAPABILITY
#define TEST_ACQUIRE(...)
#define TEST_RELEASE(...)
#define TEST_NATIVE_LOCK
#endif

/* Standard-library mutex annotations differ between host toolchains. Give
 * the fixture's native lock boundary its own portable capability type. */
class TEST_CAPABILITY KernelMutex {
 public:
  void lock() TEST_ACQUIRE() TEST_NATIVE_LOCK { mutex_.lock(); }
  void unlock() TEST_RELEASE() TEST_NATIVE_LOCK { mutex_.unlock(); }

 private:
  std::mutex mutex_;
};

typedef unsigned UBaseType_t;
struct ListItem_t {
  ListItem_t* pxNext;
  void* pvOwner;
};
struct List_t {
  UBaseType_t uxNumberOfItems;
  ListItem_t xListEnd;
};
struct TCB_t {
  uint32_t *pxStack, *pxTopOfStack;
  uint32_t ulStackSize;
  char pcTaskName[32];
  uint32_t stack[8];
  ListItem_t state, event;
};
#define configMAX_PRIORITIES 2
#define portNUM_PROCESSORS 2
#define configLIST_VOLATILE volatile
#define INCLUDE_vTaskSuspend 1
static List_t pxReadyTasksLists[configMAX_PRIORITIES];
static List_t xDelayedTaskList1, xDelayedTaskList2, xPendingReadyList[2];
static List_t xSuspendedTaskList;
static KernelMutex xKernelLock;
static thread_local unsigned lock_depth;
static std::atomic<bool> scan_started, mutation_attempted, mutation_done;
static bool force_mutation;
static unsigned ipsr, primask, basepri, allocations, frees;
static bool fail_allocation, fail_output;
static std::string output;

static void enter_kernel(KernelMutex* mutex) TEST_ACQUIRE(mutex) {
  mutex->lock();
  assert(lock_depth++ == 0u);
}
static void exit_kernel(KernelMutex* mutex) TEST_RELEASE(mutex) {
  assert(lock_depth-- == 1u);
  mutex->unlock();
}
#define prvENTER_CRITICAL(mutex) enter_kernel(mutex)
#define prvEXIT_CRITICAL(mutex) exit_kernel(mutex)
extern "C" int code_addr_is_valid(uint32_t address) {
  assert(lock_depth == 1u);
  if (force_mutation && !scan_started.exchange(true))
    while (!mutation_attempted.load()) std::this_thread::yield();
  return (address & 1u) && address >= 0x02000001u && address < 0x08000000u;
}

/* Compile the production capture bridge against controlled kernel fixtures. */
#include "h2_bk_task_trace_kernel.h"

extern "C" uint32_t __get_IPSR(void) { return ipsr; }
extern "C" uint32_t __get_PRIMASK(void) { return primask; }
extern "C" uint32_t __get_BASEPRI(void) { return basepri; }
extern "C" void* psram_malloc(size_t size) {
  assert(lock_depth == 0u);
  ++allocations;
  return fail_allocation ? nullptr : std::malloc(size);
}
extern "C" void os_free(void* memory) {
  assert(lock_depth == 0u);
  ++frees;
  std::free(memory);
}
extern "C" int h2_bk_trace_test_output(const char* format, ...) {
  assert(lock_depth == 0u);
  /* Make deletion happen before even the first output. Borrowed TCB names
   * or late stack scans would now access freed storage under ASan. */
  if (force_mutation)
    while (!mutation_done.load()) std::this_thread::yield();
  char line[512];
  va_list args;
  va_start(args, format);
  const int length = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(length >= 0 && static_cast<size_t>(length) < sizeof(line));
  output += line;
  return fail_output ? -1 : length;
}

static void reset_lists(void) {
  List_t* lists[] = {&pxReadyTasksLists[0], &pxReadyTasksLists[1],
                     &xDelayedTaskList1,    &xDelayedTaskList2,
                     &xPendingReadyList[0], &xPendingReadyList[1],
                     &xSuspendedTaskList};
  for (auto* list : lists) {
    list->uxNumberOfItems = 0u;
    list->xListEnd = {&list->xListEnd, nullptr};
  }
}
static void reset(void) {
  reset_lists();
  output.clear();
  force_mutation = fail_allocation = fail_output = false;
  ipsr = primask = basepri = allocations = frees = 0u;
  scan_started = mutation_attempted = mutation_done = false;
}
static TCB_t* task(const char* name) {
  auto* value = new TCB_t{};
  std::snprintf(value->pcTaskName, sizeof(value->pcTaskName), "%s", name);
  value->stack[1] = 0x02345679u;
  value->stack[3] = 0x02456789u;
  value->pxStack = value->stack;
  value->pxTopOfStack = value->stack + 1;
  value->ulStackSize = sizeof(value->stack);
  return value;
}
static void append(List_t* list, ListItem_t* item, TCB_t* owner) {
  auto* tail = &list->xListEnd;
  while (tail->pxNext != &list->xListEnd) tail = tail->pxNext;
  *item = {&list->xListEnd, owner};
  tail->pxNext = item;
  ++list->uxNumberOfItems;
}
static void concurrent_delete(void) {
  reset();
  auto* first = task("trace/first");
  auto* second = task("trace/second");
  append(&pxReadyTasksLists[0], &first->state, first);
  append(&xDelayedTaskList1, &second->state, second);
  append(&xPendingReadyList[0], &second->event, second);
  force_mutation = true;
  std::thread mutator([=] {
    while (!scan_started.load()) std::this_thread::yield();
    mutation_attempted = true;
    std::lock_guard<KernelMutex> lock(xKernelLock);
    reset_lists();
    delete first;
    delete second;
    mutation_done = true;
  });
  assert(h2_bk_task_trace_dump() == H2_PAL_OK);
  mutator.join();
  assert(output.find("rows=2 omitted=0 truncated=0 invalid=0") !=
         std::string::npos);
  assert(output.find("trace/first") != std::string::npos);
  assert(output.find("trace/second") != std::string::npos);
  assert(output.find("2345678 2456788") != std::string::npos);
  assert(allocations == 1 && frees == 1);
}
static void bounds_and_context(void) {
  reset();
  fail_allocation = true;
  assert(h2_bk_task_trace_dump() == H2_PAL_ERR_NO_MEMORY);
  assert(frees == 0 && output.empty());
  fail_allocation = false;
  unsigned* flags[] = {&ipsr, &primask, &basepri};
  for (auto* flag : flags) {
    *flag = 1u;
    const auto before = allocations;
    assert(h2_bk_task_trace_dump() == H2_PAL_ERR_INVALID_STATE);
    assert(allocations == before);
    *flag = 0u;
  }
  reset();
  fail_output = true;
  assert(h2_bk_task_trace_dump() == H2_PAL_ERR_IO);
  assert(allocations == 1u && frees == 1u);
  reset();
  auto* invalid = task("invalid-stack");
  invalid->pxTopOfStack = reinterpret_cast<uint32_t*>(
      reinterpret_cast<uintptr_t>(invalid->pxStack) + sizeof(invalid->stack) +
      sizeof(uint32_t));
  append(&pxReadyTasksLists[0], &invalid->state, invalid);
  assert(h2_bk_task_trace_dump() == H2_PAL_ERR_INVALID_STATE);
  assert(output.find("invalid=1") != std::string::npos);
  reset_lists();
  delete invalid;
  reset();
  auto* long_stack = task("bounded-scan");
  std::vector<uint32_t> words(H2_BK_TASK_TRACE_WORDS + 1);
  long_stack->pxStack = long_stack->pxTopOfStack = words.data();
  long_stack->ulStackSize =
      static_cast<uint32_t>(words.size() * sizeof(uint32_t));
  append(&pxReadyTasksLists[0], &long_stack->state, long_stack);
  assert(h2_bk_task_trace_dump() == H2_PAL_ERR_NO_SPACE);
  assert(output.find("truncated=1") != std::string::npos);
  reset_lists();
  delete long_stack;
  reset();
  std::vector<TCB_t*> tasks;
  for (unsigned i = 0; i < H2_BK_TASK_TRACE_TASKS + 2; ++i) {
    auto* value = task("bounded-tasks");
    append(&pxReadyTasksLists[0], &value->state, value);
    tasks.push_back(value);
  }
  assert(h2_bk_task_trace_dump() == H2_PAL_ERR_NO_SPACE);
  assert(output.find("omitted=2") != std::string::npos);
  assert(allocations == frees);
  reset_lists();
  for (auto* value : tasks) delete value;
}
int main(void) {
  for (unsigned i = 0; i < 100; ++i) concurrent_delete();
  bounds_and_context();
}
