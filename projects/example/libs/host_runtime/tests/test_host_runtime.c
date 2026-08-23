#include "h2_smoke_host_runtime.h"

#include <assert.h>
#include <stdlib.h>

struct h2_pal_queue {
  const h2_pal_mem_api_t *allocator;
};

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static int test_queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
  (void)user;
  *out_queue = h2_pal_mem_alloc(config->allocator, sizeof(**out_queue));
  if (*out_queue == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  (*out_queue)->allocator = config->allocator;
  return H2_PAL_OK;
}

static void test_queue_destroy(void *user, h2_pal_queue_t *queue) {
  (void)user;
  h2_pal_mem_free(queue->allocator, queue);
}

static int test_queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
  (void)user;
  (void)queue;
  (void)item;
  (void)timeout_ms;
  return H2_PAL_OK;
}

static int test_queue_send_latest(
    void *user,
    h2_pal_queue_t *queue,
    const void *item) {
  return test_queue_send(user, queue, item, H2_PAL_QUEUE_NO_WAIT);
}

static int test_queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
  (void)user;
  (void)queue;
  (void)out_item;
  (void)timeout_ms;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static int test_queue_reset(void *user, h2_pal_queue_t *queue) {
  (void)user;
  (void)queue;
  return H2_PAL_OK;
}

static int test_queue_close(void *user, h2_pal_queue_t *queue) {
  (void)user;
  (void)queue;
  return H2_PAL_OK;
}

int main(void) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  static const h2_pal_mem_api_t mem = {
      .user = NULL,
      .vtable = &mem_vtable,
  };
  static const h2_pal_queue_vtable_t queue_vtable = {
      .create = test_queue_create,
      .destroy = test_queue_destroy,
      .send = test_queue_send,
      .send_latest = test_queue_send_latest,
      .recv = test_queue_recv,
      .reset = test_queue_reset,
      .close = test_queue_close,
  };
  static const h2_pal_queue_api_t queue = {
      .user = NULL,
      .vtable = &queue_vtable,
  };
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "host-test",
      "native",
      "test",
      &mem,
      h2_pal_unsupported_time_api(),
      &queue,
      h2_pal_unsupported_display_api());
  assert(config.touch == h2_pal_unsupported_touch_api());

  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  assert(runtime != NULL);
  assert(runtime->touch != NULL);
  assert(runtime->touch->vtable == config.touch->vtable);
  h2_runtime_deinit(runtime);
  return 0;
}
