#include "h2_javascript.h"

#include "h2_javascript_quickjs.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef union javascript_block {
  max_align_t alignment;
  size_t size;
} javascript_block_t;

typedef struct javascript_run {
  h2_javascript_config_t config;
  uint32_t interruptions;
  bool allocation_failed, interrupted;
  int native_result;
} javascript_run_t;

static void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

static void* allocate(JSMallocState* state, size_t size) {
  javascript_run_t* run = state->opaque;
  size_t limit = state->malloc_limit < run->config.memory_limit
                     ? state->malloc_limit
                     : run->config.memory_limit;
  if (size > SIZE_MAX - sizeof(javascript_block_t) ||
      state->malloc_size > limit || size > limit - state->malloc_size) {
    run->allocation_failed = true;
    return NULL;
  }
  javascript_block_t* block =
      h2_pal_mem_alloc(run->config.memory, sizeof(*block) + size);
  if (block == NULL) {
    run->allocation_failed = true;
    return NULL;
  }
  block->size = size;
  state->malloc_size += size;
  ++state->malloc_count;
  return block + 1;
}

static void release(JSMallocState* state, void* pointer) {
  if (pointer == NULL) return;
  javascript_run_t* run = state->opaque;
  javascript_block_t* block = (javascript_block_t*)pointer - 1;
  state->malloc_size -= block->size;
  --state->malloc_count;
  wipe(block, sizeof(*block) + block->size);
  h2_pal_mem_free(run->config.memory, block);
}

static void* resize(JSMallocState* state, void* pointer, size_t size) {
  if (pointer == NULL) return size != 0u ? allocate(state, size) : NULL;
  if (size == 0u) {
    release(state, pointer);
    return NULL;
  }
  javascript_block_t* block = (javascript_block_t*)pointer - 1;
  size_t old_size = block->size;
  state->malloc_size -= old_size;
  void* replacement = allocate(state, size);
  state->malloc_size += old_size;
  if (replacement != NULL) {
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    release(state, pointer);
  }
  return replacement;
}

static size_t allocation_size(const void* pointer) {
  return pointer != NULL ? ((const javascript_block_t*)pointer - 1)->size : 0u;
}

static int interrupt(JSRuntime* runtime, void* user) {
  (void)runtime;
  javascript_run_t* run = user;
  if (run->interruptions == run->config.interrupt_limit) {
    run->interrupted = true;
    return 1;
  }
  ++run->interruptions;
  return 0;
}

static JSValue native_call(JSContext* context, JSValueConst receiver, int count,
                           JSValueConst* arguments) {
  (void)receiver;
  javascript_run_t* run = JS_GetContextOpaque(context);
  if (count != 2 || !JS_IsString(arguments[0]) || !JS_IsString(arguments[1]))
    return JS_ThrowTypeError(context, "native arguments");
  if (run->config.native == NULL) {
    run->native_result = H2_PAL_ERR_UNSUPPORTED;
    return JS_ThrowInternalError(context, "native unavailable");
  }
  const char* method = JS_ToCString(context, arguments[0]);
  const char* parameters = JS_ToCString(context, arguments[1]);
  char* result = NULL;
  size_t length = 0u;
  int rc = method != NULL && parameters != NULL
               ? run->config.native(run->config.user, method, parameters,
                                    run->config.memory, &result, &length)
               : H2_PAL_ERR_NO_MEMORY;
  if (rc == H2_PAL_OK && ((result == NULL && length != 0u) || length > 16384u))
    rc = H2_PAL_ERR_FORMAT;
  JSValue value =
      rc == H2_PAL_OK
          ? JS_NewStringLen(context, result != NULL ? result : "", length)
          : JS_ThrowInternalError(context, "native failure");
  if (result != NULL) {
    wipe(result, length);
    h2_pal_mem_free(run->config.memory, result);
  }
  JS_FreeCString(context, method);
  JS_FreeCString(context, parameters);
  if (rc != H2_PAL_OK) run->native_result = rc;
  return value;
}

h2_pal_result_t h2_javascript_evaluate(const h2_javascript_config_t* config,
                                       const char* source, size_t source_length,
                                       uint8_t* output, size_t capacity,
                                       size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (config == NULL || config->memory == NULL ||
      config->memory->vtable == NULL || config->memory->vtable->alloc == NULL ||
      config->memory->vtable->free == NULL || config->time == NULL ||
      source == NULL || source_length == 0u || source_length == SIZE_MAX ||
      memchr(source, '\0', source_length) != NULL ||
      (output == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  javascript_run_t run = {.config = *config};
  if (run.config.memory_limit == 0u)
    run.config.memory_limit = 2u * 1024u * 1024u;
  if (run.config.stack_limit == 0u) run.config.stack_limit = 128u * 1024u;
  if (run.config.interrupt_limit == 0u) run.config.interrupt_limit = 10000u;
  uint64_t timestamp = 0u;
  int rc = h2_pal_time_get_wall_ms(config->time, &timestamp);
  if (rc != H2_PAL_OK) return rc;
  if (timestamp > INT64_MAX / 1000u) return H2_PAL_ERR_INVALID_ARG;
  static const JSMallocFunctions allocation = {allocate, release, resize,
                                               allocation_size};
  JSRuntime* runtime = JS_NewRuntime2(&allocation, &run);
  if (runtime == NULL) return H2_PAL_ERR_NO_MEMORY;
  JS_SetTimeMicroseconds(runtime, (int64_t)(timestamp * 1000u));
  JS_SetMemoryLimit(runtime, run.config.memory_limit);
  JS_SetMaxStackSize(runtime, run.config.stack_limit);
  JS_SetInterruptHandler(runtime, interrupt, &run);
  JSContext* context = JS_NewContext(runtime);
  char* copied = NULL;
  JSValue value = JS_UNDEFINED;
  const char* text = NULL;
  size_t size = 0u;
  rc = H2_PAL_ERR_NO_MEMORY;
  if (context == NULL || run.allocation_failed) goto done;
  JS_SetContextOpaque(context, &run);
  JSValue global = JS_GetGlobalObject(context);
  JSValue function = JS_NewCFunction(context, native_call, "__h2_native", 2);
  int registered = JS_SetPropertyStr(context, global, "__h2_native", function);
  JS_FreeValue(context, global);
  if (registered < 0 || run.allocation_failed) goto done;
  copied = h2_pal_mem_alloc(config->memory, source_length + 1u);
  if (copied == NULL) goto done;
  memcpy(copied, source, source_length);
  copied[source_length] = '\0';
  value =
      JS_Eval(context, copied, source_length, "injected", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(value)) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    rc = H2_PAL_ERR_NOT_FOUND;
    goto done;
  }
  text = JS_ToCStringLen(context, &size, value);
  if (text == NULL) goto done;
  if (run.native_result != 0 || run.allocation_failed || run.interrupted) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  *length = size;
  if (size > capacity) {
    rc = H2_PAL_ERR_NO_SPACE;
  } else {
    if (size != 0u) memcpy(output, text, size);
    rc = H2_PAL_OK;
  }
done:
  if (copied != NULL) {
    wipe(copied, source_length);
    h2_pal_mem_free(config->memory, copied);
  }
  if (context != NULL) {
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);
    JS_FreeContext(context);
  }
  JS_FreeRuntime(runtime);
  if (run.native_result != 0) rc = run.native_result;
  if (run.allocation_failed) rc = H2_PAL_ERR_NO_MEMORY;
  if (run.interrupted) rc = H2_PAL_ERR_TIMEOUT;
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NO_SPACE) *length = 0u;
  return rc;
}
