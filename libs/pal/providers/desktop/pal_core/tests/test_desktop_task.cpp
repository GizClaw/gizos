#include "h2_desktop_platform.h"
#include "h2_atomic.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
struct Heap {
  size_t live = 0;
  size_t bytes = 0;
  bool fail = false;
};
void *alloc(void *user, size_t bytes) {
  auto *heap = static_cast<Heap *>(user);
  if (heap->fail)
    return nullptr;
  heap->bytes = bytes;
  ++heap->live;
  return std::malloc(bytes);
}
void release(void *user, void *ptr) {
  if (ptr != nullptr) {
    --static_cast<Heap *>(user)->live;
    std::free(ptr);
  }
}
h2_pal_result_t resolve(void *, const h2_pal_task_options_t *options,
                        size_t *out) {
  if (options == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  *out =
      std::strcmp(options->name, "internal") == 0
          ? 0
          : (options->min_stack_size < 16384 ? 16384 : options->min_stack_size);
  return H2_PAL_OK;
}
void entry(void *user) {
  auto *runs = static_cast<h2_atomic_uint_t *>(user);
  (void)h2_atomic_uint_fetch_add(runs, 1u, H2_ATOMIC_SEQ_CST);
}
struct SelfJoin {
  const h2_pal_task_api_t *api;
  h2_atomic_ptr_t task{};
  h2_atomic_int_t result{};
};
void self_join(void *user) {
  auto *state = static_cast<SelfJoin *>(user);
  h2_pal_task_t *task = nullptr;
  while ((task = static_cast<h2_pal_task_t *>(
              h2_atomic_ptr_load(&state->task, H2_ATOMIC_SEQ_CST))) == nullptr)
    std::this_thread::yield();
  h2_atomic_int_store(&state->result, h2_pal_task_join(state->api, task),
                      H2_ATOMIC_SEQ_CST);
}
} // namespace
int main() {
  Heap heap;
  const h2_pal_mem_vtable_t vtable = {alloc, nullptr, release};
  const h2_pal_mem_api_t mem = {&heap, &vtable};
  const h2_desktop_task_stack_config_t config = {&mem, resolve, nullptr};
  assert(h2_desktop_platform_configure_task_stacks(&config) == H2_PAL_OK);
  const auto *api = h2_desktop_platform_task_api();
  h2_atomic_uint_t runs{};
  assert(h2_atomic_uint_init(&runs, 0u) == H2_ATOMIC_OK);
  h2_pal_task_t *task = nullptr;
  h2_pal_task_options_t options = {"psram", 4096};
  assert(h2_pal_task_start(api, &options, entry, &runs, &task) == H2_PAL_OK);
  assert(heap.live == 1 && heap.bytes == 16384);
  assert(h2_desktop_platform_configure_task_stacks(nullptr) == H2_PAL_ERR_BUSY);
  assert(h2_pal_task_join(api, task) == H2_PAL_OK);
  assert(heap.live == 0 && h2_atomic_uint_load(&runs, H2_ATOMIC_SEQ_CST) == 1u);
  options.min_stack_size = 65536;
  assert(h2_pal_task_start(api, &options, entry, &runs, &task) == H2_PAL_OK);
  assert(heap.bytes == 65536);
  assert(h2_pal_task_join(api, task) == H2_PAL_OK);
  options.name = "internal";
  assert(h2_pal_task_start(api, &options, entry, &runs, &task) == H2_PAL_OK);
  assert(heap.live == 0);
  assert(h2_pal_task_join(api, task) == H2_PAL_OK);
  options.name = "psram";
  SelfJoin self{api};
  assert(h2_atomic_ptr_init(&self.task, nullptr) == H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&self.result, 999) == H2_ATOMIC_OK);
  assert(h2_pal_task_start(api, &options, self_join, &self, &task) ==
         H2_PAL_OK);
  h2_atomic_ptr_store(&self.task, task, H2_ATOMIC_SEQ_CST);
  while (h2_atomic_int_load(&self.result, H2_ATOMIC_SEQ_CST) == 999)
    std::this_thread::yield();
  assert(h2_atomic_int_load(&self.result, H2_ATOMIC_SEQ_CST) ==
             H2_PAL_ERR_INVALID_STATE && heap.live == 1);
  assert(h2_desktop_platform_configure_task_stacks(nullptr) == H2_PAL_ERR_BUSY);
  assert(h2_pal_task_join(api, task) == H2_PAL_OK && heap.live == 0);
  heap.fail = true;
  options.name = "psram";
  assert(h2_pal_task_start(api, &options, entry, &runs, &task) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(task == nullptr && h2_atomic_uint_load(&runs, H2_ATOMIC_SEQ_CST) == 3u && heap.live == 0);
  assert(h2_pal_task_start(api, nullptr, entry, &runs, &task) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(task == nullptr);
  assert(h2_desktop_platform_configure_task_stacks(nullptr) == H2_PAL_OK);
  assert(h2_pal_task_start(api, nullptr, entry, &runs, &task) == H2_PAL_OK);
  assert(h2_pal_task_join(api, task) == H2_PAL_OK);
  assert(h2_atomic_uint_load(&runs, H2_ATOMIC_SEQ_CST) == 4u);
  h2_desktop_task_stack_config_t bad{};
  assert(h2_desktop_platform_configure_task_stacks(&bad) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_atomic_ptr_destroy(&self.task);
  h2_atomic_int_destroy(&self.result);
  h2_atomic_uint_destroy(&runs);
}
