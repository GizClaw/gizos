#include "h2_memory_pref.h"
#include "h2_app_test_mem.h"

#include <assert.h>
#include <string.h>

static void expect(h2_pal_pref_namespace_t* ns, const h2_pal_mem_api_t* mem,
                   const char* key, const char* expected) {
  void* data = NULL;
  size_t length = 0u;
  int rc = ns->get_blob(ns, mem, key, &data, &length);
  if (expected == NULL) {
    assert(rc == H2_PAL_ERR_NOT_FOUND && data == NULL && length == 0u);
  } else {
    assert(rc == 0 && length == strlen(expected));
    assert(memcmp(data, expected, length) == 0);
    h2_pal_mem_free(mem, data);
  }
}

int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  h2_memory_pref_t* store = NULL;
  h2_memory_pref_config_t config = {&mem.api, 2u, 3u, 256u, 3u};
  assert(h2_memory_pref_create(NULL, &store) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_memory_pref_create(&config, &store) == 0);
  const h2_pal_pref_api_t* api = h2_memory_pref_api(store);
  h2_pal_pref_namespace_t *writer, *reader, *second;
  assert(h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_ONLY, &reader) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_WRITE, &writer) == 0);
  assert(h2_memory_pref_destroy(&store) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_WRITE, &second) ==
         H2_PAL_ERR_BUSY);
  assert(h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_ONLY, &reader) == 0);
  assert(writer->set_blob(NULL, "value", "x", 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(writer->close(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(writer->commit(NULL) == H2_PAL_ERR_INVALID_ARG);
  void* invalid = (void*)1;
  size_t invalid_length = 1u;
  assert(writer->get_blob(NULL, &mem.api, "value", &invalid, &invalid_length) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(invalid == NULL && invalid_length == 0u);
  assert(writer->set_blob(writer, "value", "old", 3u) == 0);
  expect(writer, &mem.api, "value", "old");
  expect(reader, &mem.api, "value", NULL);
  assert(writer->commit(writer) == 0);
  expect(reader, &mem.api, "value", "old");
  assert(writer->set_blob(writer, "value", "new", 3u) == 0);
  mem.fail_at = mem.calls + 1u;
  assert(writer->commit(writer) == H2_PAL_ERR_NO_MEMORY);
  expect(reader, &mem.api, "value", "old");
  expect(writer, &mem.api, "value", "new");
  assert(writer->close(writer) == 0); /* rollback */
  expect(reader, &mem.api, "value", "old");
  assert(reader->set_blob(reader, "x", "x", 1u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_WRITE, &writer) == 0);
  mem.fail_at = mem.calls + 1u;
  assert(writer->set_blob(writer, "value", "bad", 3u) == H2_PAL_ERR_NO_MEMORY);
  expect(writer, &mem.api, "value", "old");
  assert(writer->set_u32(writer, "number", 123456u) == 0);
  assert(writer->set_string(writer, "name", "TEST") == 0);
  assert(writer->set_blob(writer, "overflow", "x", 1u) == H2_PAL_ERR_NO_SPACE);
  assert(writer->commit(writer) == 0);
  uint32_t number;
  assert(reader->get_u32(reader, "number", &number) == 0 && number == 123456u);
  assert(reader->get_u32(reader, "name", &number) == H2_PAL_ERR_FORMAT &&
         number == 0u);
  char* name = NULL;
  assert(reader->get_string(reader, &mem.api, "name", &name) == 0 &&
         strcmp(name, "TEST") == 0);
  h2_pal_mem_free(&mem.api, name);
  h2_pal_pref_cursor_t* cursor = NULL;
  h2_pal_pref_entry_t entry;
  unsigned count = 0u;
  while (reader->iterate(reader, &cursor, &entry) == 0) ++count;
  assert(count == 3u && cursor == NULL);
  assert(reader->iterate(reader, &cursor, &entry) == 0);
  h2_pal_pref_cursor_t* second_cursor = NULL;
  assert(reader->iterate(reader, &second_cursor, &entry) == H2_PAL_ERR_BUSY &&
         second_cursor == NULL);
  assert(reader->close(reader) == H2_PAL_ERR_INVALID_STATE);
  assert(reader->iterate_close(reader, &cursor) == 0 && cursor == NULL);
  assert(reader->close(reader) == 0);
  assert(writer->clear(writer) == 0 && writer->commit(writer) == 0);
  assert(writer->set_bool(writer, "bool", 5) == 0);
  int boolean;
  assert(writer->get_bool(writer, "bool", &boolean) == 0 && boolean == 1);
  assert(writer->set_i32(writer, "signed", -42) == 0);
  int32_t signed_value;
  assert(writer->get_i32(writer, "signed", &signed_value) == 0 &&
         signed_value == -42);
  assert(writer->remove(writer, "bool") == 0);
  assert(writer->remove(writer, "bool") == H2_PAL_ERR_NOT_FOUND);
  assert(writer->set_blob(writer, "empty", NULL, 0u) == 0);
  assert(writer->commit(writer) == 0);
  assert(writer->close(writer) == 0);
  assert(h2_pal_pref_open(api, "B", H2_PAL_PREF_OPEN_READ_WRITE, &writer) == 0);
  assert(h2_pal_pref_open(api, "C", H2_PAL_PREF_OPEN_READ_WRITE, &second) ==
         H2_PAL_ERR_NO_SPACE);
  assert(writer->close(writer) == 0);
  assert(h2_memory_pref_destroy(&store) == 0 && store == NULL);
  assert(mem.live_blocks == 0u);
  assert(h2_memory_pref_create(&config, &store) == 0);
  assert(h2_pal_pref_open(h2_memory_pref_api(store), "A",
                          H2_PAL_PREF_OPEN_READ_ONLY,
                          &reader) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_memory_pref_destroy(&store) == 0);
  assert(mem.live_blocks == 0u);
  /* Fail every allocation in a complete transaction and check teardown. */
  for (size_t failure = 1u; failure <= 16u; ++failure) {
    h2_app_test_mem_init(&mem, NULL);
    mem.fail_at = failure;
    if (h2_memory_pref_create(&config, &store) == 0) {
      api = h2_memory_pref_api(store);
      if (h2_pal_pref_open(api, "A", H2_PAL_PREF_OPEN_READ_WRITE, &writer) ==
          0) {
        if (writer->set_blob(writer, "value", "data", 4u) == 0)
          (void)writer->commit(writer);
        (void)writer->set_blob(writer, "value", "replacement", 11u);
        (void)writer->commit(writer);
        assert(writer->close(writer) == 0);
      }
      assert(h2_memory_pref_destroy(&store) == 0);
    }
    assert(mem.live_blocks == 0u);
  }
  return 0;
}
