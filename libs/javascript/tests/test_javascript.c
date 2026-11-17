#include "h2_javascript.h"

#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned native_calls;
static int native_result;

static int native(void* user, const char* method, const char* parameters,
                  const h2_pal_mem_api_t* memory, char** result,
                  size_t* length) {
  (void)user;
  assert(strcmp(method, "fixture") == 0);
  assert(strcmp(parameters, "{\"value\":7}") == 0);
  ++native_calls;
  *result = h2_pal_mem_alloc(memory, 4u);
  if (*result == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(*result, "ABC", 4u);
  *length = 3u;
  return native_result;
}

static void check_unchanged(uint8_t* output, size_t size) {
  for (size_t index = 0u; index < size; ++index) assert(output[index] == 0xa5u);
}

int main(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000123u) == 0);
  h2_javascript_config_t config = {.memory = &memory.api,
                                   .time = &time.api,
                                   .native = native,
                                   .stack_limit = 1024u * 1024u};
  static const struct {
    const char* source;
    const char* expected;
  } cases[] = {
      {"1 + 2 * 3", "7"},
      {"JSON.stringify({value:[1,2,3].map(x=>x*2)})", "{\"value\":[2,4,6]}"},
      {"Date.now().toString()", "1794449000123"},
      {"new Date().toISOString()", "2026-11-12T02:03:20.123Z"},
      {"__h2_native('fixture',JSON.stringify({value:7}))", "ABC"},
      {"'ABC'.replace(/B/g,'-')", "A-C"},
      {"'x'.repeat(3)", "xxx"},
      {"JSON.stringify({global:typeof process, atomics:typeof Atomics})",
       "{\"global\":\"undefined\",\"atomics\":\"undefined\"}"}};
  uint8_t output[256];
  for (size_t index = 0u; index < sizeof(cases) / sizeof(*cases); ++index) {
    memset(output, 0xa5, sizeof(output));
    size_t length = 999u;
    int rc = h2_javascript_evaluate(&config, cases[index].source,
                                    strlen(cases[index].source), output + 1u,
                                    sizeof(output) - 2u, &length);
    if (rc != 0) fprintf(stderr, "JavaScript case=%zu result=%d\n", index, rc);
    assert(rc == 0);
    assert(length == strlen(cases[index].expected));
    if (memcmp(output + 1u, cases[index].expected, length) != 0)
      fprintf(stderr, "JavaScript case=%zu output=%.*s expected=%s\n", index,
              (int)length, output + 1u, cases[index].expected);
    assert(memcmp(output + 1u, cases[index].expected, length) == 0);
    assert(output[0] == 0xa5u && output[length + 1u] == 0xa5u);
    assert(memory.live_blocks == 0u);
  }
  assert(native_calls == 1u);
  size_t length = 0u;
  memset(output, 0xa5, sizeof(output));
  assert(h2_javascript_evaluate(&config, "'text'", 6u, output, 3u, &length) ==
         H2_PAL_ERR_NO_SPACE);
  assert(length == 4u);
  check_unchanged(output, sizeof(output));
  assert(h2_javascript_evaluate(&config, "'text'", 6u, NULL, 0u, &length) ==
         H2_PAL_ERR_NO_SPACE);
  assert(length == 4u);
  assert(h2_javascript_evaluate(&config, "null", 4u, output, sizeof(output),
                                &length) == H2_PAL_ERR_NOT_FOUND);
  assert(length == 0u);
  assert(h2_javascript_evaluate(&config, "throw new Error('fixture')", 26u,
                                output, sizeof(output),
                                &length) == H2_PAL_ERR_FORMAT);
  check_unchanged(output, sizeof(output));
  config.interrupt_limit = 2u;
  const char endless[] = "while(true) {}";
  assert(h2_javascript_evaluate(&config, endless, sizeof(endless) - 1u, output,
                                sizeof(output), &length) == H2_PAL_ERR_TIMEOUT);
  assert(length == 0u && memory.live_blocks == 0u);
  config.interrupt_limit = 0u;
  config.memory_limit = 1u;
  assert(h2_javascript_evaluate(&config, "1", 1u, output, sizeof(output),
                                &length) == H2_PAL_ERR_NO_MEMORY);
  assert(memory.live_blocks == 0u);
  config.memory_limit = 0u;
  size_t start = memory.calls;
  assert(h2_javascript_evaluate(&config, "1", 1u, output, sizeof(output),
                                &length) == 0);
  size_t allocations = memory.calls - start;
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    memory.fail_at = memory.calls + fail;
    memset(output, 0xa5, sizeof(output));
    int rc = h2_javascript_evaluate(&config, "1", 1u, output, sizeof(output),
                                    &length);
    memory.fail_at = 0u;
    if (rc != H2_PAL_ERR_NO_MEMORY)
      fprintf(stderr, "JavaScript allocation=%zu result=%d\n", fail, rc);
    assert(rc == H2_PAL_ERR_NO_MEMORY);
    assert(length == 0u && memory.live_blocks == 0u);
    check_unchanged(output, sizeof(output));
  }
  const char native_script[] =
      "try { __h2_native('fixture','{\"value\":7}') } catch(e) {'caught'}";
  native_result = H2_PAL_ERR_IO;
  assert(h2_javascript_evaluate(&config, native_script,
                                sizeof(native_script) - 1u, output,
                                sizeof(output), &length) == H2_PAL_ERR_IO);
  assert(length == 0u && memory.live_blocks == 0u);
  check_unchanged(output, sizeof(output));
  printf("JavaScript allocation failure points=%zu\n", allocations);
  return 0;
}
