#include "h2_esp_libco_xtensa_internal.h"

#include "libco.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The pinned ESP-IDF ABI exports this function with a pointer return type. */
extern void *xTaskGetCurrentTaskHandle(void);

_Static_assert(sizeof(void *) == sizeof(uint32_t),
               "ESP32-S3 libco context requires 32-bit pointers");
_Static_assert(H2_ESP_LIBCO_STACK_ALIGNMENT == 16,
               "ESP32-S3 windowed ABI requires 16-byte stacks");
_Static_assert(sizeof(h2_esp_libco_xtensa_context_t) == H2_ESP_LIBCO_CTX_SIZE,
               "Xtensa context size must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, sp) ==
                   H2_ESP_LIBCO_CTX_SP,
               "Xtensa SP offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, a0) ==
                   H2_ESP_LIBCO_CTX_A0,
               "Xtensa A0 offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, ps) ==
                   H2_ESP_LIBCO_CTX_PS,
               "Xtensa PS offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, sar) ==
                   H2_ESP_LIBCO_CTX_SAR,
               "Xtensa SAR offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, lbeg) ==
                   H2_ESP_LIBCO_CTX_LBEG,
               "Xtensa LBEG offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, lend) ==
                   H2_ESP_LIBCO_CTX_LEND,
               "Xtensa LEND offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, lcount) ==
                   H2_ESP_LIBCO_CTX_LCOUNT,
               "Xtensa LCOUNT offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, stack_top) ==
                   H2_ESP_LIBCO_CTX_STACK_TOP,
               "Xtensa stack-top offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, entrypoint) ==
                   H2_ESP_LIBCO_CTX_ENTRYPOINT,
               "Xtensa entrypoint offset must match assembly");
_Static_assert(offsetof(h2_esp_libco_xtensa_context_t, started) ==
                   H2_ESP_LIBCO_CTX_STARTED,
               "Xtensa started offset must match assembly");

static h2_esp_libco_xtensa_context_t s_root_context;
static h2_esp_libco_xtensa_context_t *s_active_context;
static void *s_owner_task;
static uint32_t s_owner_core = UINT32_MAX;

static uint32_t h2_esp_libco_core_id(void) {
  uint32_t core_id;
  __asm__ volatile("rsr.prid %0\n"
                   "extui %0, %0, 13, 1"
                   : "=r"(core_id));
  return core_id;
}

static void h2_esp_libco_bind_or_check_owner(void) {
  void *task = xTaskGetCurrentTaskHandle();
  uint32_t core = h2_esp_libco_core_id();
  if (s_owner_task == NULL) {
    s_owner_task = task;
    s_owner_core = core;
    return;
  }
  if (s_owner_task != task || s_owner_core != core) {
    abort();
  }
}

cothread_t co_active(void) {
  h2_esp_libco_bind_or_check_owner();
  if (s_active_context == NULL) {
    memset(&s_root_context, 0, sizeof(s_root_context));
    s_root_context.started = 1u;
    s_active_context = &s_root_context;
  }
  return s_active_context;
}

cothread_t co_derive(void *memory, unsigned int size,
                     void (*entrypoint)(void)) {
  uintptr_t base = (uintptr_t)memory;
  uintptr_t top;
  h2_esp_libco_xtensa_context_t *context;
  h2_esp_libco_bind_or_check_owner();
  if (memory == NULL || entrypoint == NULL ||
      base % H2_ESP_LIBCO_STACK_ALIGNMENT != 0u ||
      size < H2_ESP_LIBCO_CTX_SIZE + 128u) {
    return NULL;
  }
  top = (base + size) & ~(uintptr_t)(H2_ESP_LIBCO_STACK_ALIGNMENT - 1u);
  if (top <= base + H2_ESP_LIBCO_CTX_SIZE + 64u) {
    return NULL;
  }
  context = memory;
  memset(context, 0, sizeof(*context));
  context->stack_top = (uint32_t)top;
  context->entrypoint = (uint32_t)(uintptr_t)entrypoint;
  return context;
}

void co_switch(cothread_t handle) {
  h2_esp_libco_xtensa_context_t *next = handle;
  h2_esp_libco_xtensa_context_t *previous;
  h2_esp_libco_bind_or_check_owner();
  if (next == NULL || next == s_active_context) {
    abort();
  }
  previous = s_active_context;
  s_active_context = next;
  h2_esp_libco_xtensa_swap(next, previous);
}
