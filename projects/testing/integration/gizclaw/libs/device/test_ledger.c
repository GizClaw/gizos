#include "ledger.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char line[] =
    "H2_GIZCLAW_E2E stage=coverage-begin case=resource\n";

typedef struct test_memory {
  size_t calls, frees, live, bytes, peak_bytes, fail_call;
} test_memory_t;

typedef union test_allocation {
  long double alignment;
  void* pointer_alignment;
  size_t size;
} test_allocation_t;

static void* test_alloc(void* user, size_t size) {
  test_memory_t* memory = user;
  /* A growth allocation is one page, never the full logical capacity. */
  assert(size >= H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES);
  assert(size <= H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES + 64u);
  ++memory->calls;
  if (memory->calls == memory->fail_call) return NULL;
  test_allocation_t* allocation = malloc(sizeof(*allocation) + size);
  assert(allocation != NULL);
  allocation->size = size;
  ++memory->live;
  memory->bytes += size;
  if (memory->bytes > memory->peak_bytes) memory->peak_bytes = memory->bytes;
  return allocation + 1;
}

static void test_free(void* user, void* ptr) {
  test_memory_t* memory = user;
  test_allocation_t* allocation = (test_allocation_t*)ptr - 1;
  assert(memory->live > 0u && memory->bytes >= allocation->size);
  --memory->live;
  memory->bytes -= allocation->size;
  ++memory->frees;
  free(allocation);
}

static const h2_pal_mem_vtable_t test_memory_vtable = {.alloc = test_alloc,
                                                       .free = test_free};

static h2_pal_mem_api_t memory_api(test_memory_t* memory) {
  return (h2_pal_mem_api_t){.user = memory, .vtable = &test_memory_vtable};
}

static void make_record(char* record, size_t size) {
  static const char prefix[] = "H2_GIZCLAW_E2E ";
  assert(size >= sizeof(prefix));
  memset(record, 'x', size);
  memcpy(record, prefix, sizeof(prefix) - 1u);
  record[size - 1u] = '\n';
}

static void assert_replay(const h2_gizclaw_e2e_ledger_t* ledger,
                          const char* expected, size_t expected_size) {
  assert(ledger->size == expected_size);
  const char* data = NULL;
  size_t size = 0u;
  for (size_t offset = 0u; offset < expected_size;) {
    assert(h2_gizclaw_e2e_ledger_span(ledger, offset, &data, &size) ==
           H2_PAL_OK);
    assert(data != NULL && size > 0u && size <= expected_size - offset);
    assert(memcmp(data, expected + offset, size) == 0);
    offset += size;
  }
  assert(h2_gizclaw_e2e_ledger_span(ledger, expected_size, &data, &size) ==
         H2_PAL_OK);
  assert(data == NULL && size == 0u);
  assert(h2_gizclaw_e2e_ledger_span(ledger, expected_size + 1u, &data, &size) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_borrowed(void) {
  char buffer[512] = {0};
  h2_gizclaw_e2e_ledger_t ledger;
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_OK);
  assert(ledger.records == 1u && ledger.size == sizeof(line) - 1u);
  assert(memcmp(ledger.data, line, ledger.size) == 0);
  assert(ledger.crc32 == 0xf03ce93fu); /* independent Python zlib vector */
  assert_replay(&ledger, line, sizeof(line) - 1u);
  uint32_t checksum = ledger.crc32;
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error != H2_PAL_OK && ledger.crc32 == checksum &&
         ledger.records == 1u);
  assert_replay(&ledger, line, sizeof(line) - 1u);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  assert(memcmp(buffer, line, sizeof(line) - 1u) == 0);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, 10u) == H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_ERR_NO_SPACE && ledger.size == 0u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_NO_SPACE);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) ==
         H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, NULL, 0u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) != H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_init(&ledger, buffer, sizeof(buffer)) ==
         H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 2u);
  assert(ledger.error != H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_init(NULL, buffer, sizeof(buffer)) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
}

static void test_cross_page_copy_and_replay(void) {
  test_memory_t memory = {0};
  h2_pal_mem_api_t allocator = memory_api(&memory);
  char record[H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES + 17u];
  char expected[sizeof(record) + sizeof(line) - 1u];
  h2_gizclaw_e2e_ledger_t ledger, borrowed;
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator,
                                          sizeof(expected)) == H2_PAL_OK);
  assert(memory.calls == 0u && ledger.data == NULL);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_INVALID_STATE);
  const char* span = NULL;
  size_t span_size = 0u;
  assert(h2_gizclaw_e2e_ledger_span(&ledger, 0u, &span, &span_size) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_ledger_init(&borrowed, expected, sizeof(expected)) ==
         H2_PAL_OK);
  make_record(record, sizeof(record));
  h2_gizclaw_e2e_ledger_append(&ledger, record, sizeof(record));
  h2_gizclaw_e2e_ledger_append(&borrowed, record, sizeof(record));
  memset(record, '?', sizeof(record)); /* append owns the original bytes */
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  h2_gizclaw_e2e_ledger_append(&borrowed, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_OK && ledger.records == 2u &&
         memory.calls == 2u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_ledger_freeze(&borrowed) == H2_PAL_OK);
  assert(ledger.crc32 == borrowed.crc32);
  assert_replay(&ledger, expected, sizeof(expected));
  assert_replay(&ledger, expected, sizeof(expected));
  assert(h2_gizclaw_e2e_ledger_span(&ledger,
                                    H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES - 1u,
                                    &span, &span_size) == H2_PAL_OK);
  assert(span_size == 1u &&
         *span == expected[H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES - 1u]);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_ERR_INVALID_STATE && ledger.records == 2u);
  assert(ledger.crc32 == borrowed.crc32 && memory.calls == 2u);
  assert_replay(&ledger, expected, sizeof(expected));
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  assert(memory.live == 0u && memory.bytes == 0u && memory.frees == 2u);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  h2_gizclaw_e2e_ledger_deinit(&borrowed);
  assert(memory.frees == 2u);
}

static void test_original_capacity_and_overflow(void) {
  enum { capacity = 384u * 1024u };
  static char expected[capacity];
  char record[128];
  make_record(record, sizeof(record));
  test_memory_t memory = {0};
  h2_pal_mem_api_t allocator = memory_api(&memory);
  h2_gizclaw_e2e_ledger_t ledger, borrowed;
  assert(h2_gizclaw_e2e_ledger_init(&borrowed, expected, sizeof(expected)) ==
         H2_PAL_OK);
  for (unsigned round = 0u; round < 2u; ++round) {
    assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator, capacity) ==
           H2_PAL_OK);
    for (size_t i = 0u; i < capacity / sizeof(record); ++i) {
      h2_gizclaw_e2e_ledger_append(&ledger, record, sizeof(record));
      if (round == 0u)
        h2_gizclaw_e2e_ledger_append(&borrowed, record, sizeof(record));
    }
    assert(ledger.error == H2_PAL_OK && ledger.size == capacity);
    assert(ledger.records == capacity / sizeof(record));
    assert(memory.live == capacity / H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES);
    assert(memory.peak_bytes <= capacity + 96u * 64u);
    size_t calls = memory.calls;
    if (round == 0u) {
      assert(h2_gizclaw_e2e_ledger_freeze(&borrowed) == H2_PAL_OK);
      assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_OK);
      assert(ledger.crc32 == borrowed.crc32);
      assert_replay(&ledger, expected, capacity);
    } else {
      h2_gizclaw_e2e_ledger_append(&ledger, record, sizeof(record));
      assert(ledger.error == H2_PAL_ERR_NO_SPACE && ledger.size == capacity);
      assert(ledger.records == capacity / sizeof(record));
      assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_NO_SPACE);
      assert(!ledger.frozen);
    }
    assert(memory.calls ==
           calls); /* freeze, replay and overflow allocate nothing */
    h2_gizclaw_e2e_ledger_deinit(&ledger);
    assert(memory.live == 0u && memory.bytes == 0u);
  }
  h2_gizclaw_e2e_ledger_deinit(&borrowed);
  /* Physical page slack cannot extend a smaller logical capacity. */
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator,
                                          sizeof(line) - 1u) == H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_ERR_NO_SPACE && ledger.records == 1u);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  assert(memory.live == 0u && memory.bytes == 0u);
}

static void test_allocation_failures(void) {
  test_memory_t memory = {.fail_call = 1u};
  h2_pal_mem_api_t allocator = memory_api(&memory);
  h2_gizclaw_e2e_ledger_t ledger;
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator, 384u * 1024u) ==
         H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(ledger.error == H2_PAL_ERR_NO_MEMORY && ledger.size == 0u &&
         ledger.records == 0u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_NO_MEMORY);
  assert(!ledger.frozen && memory.live == 0u);
  h2_gizclaw_e2e_ledger_deinit(&ledger);

  memory = (test_memory_t){0};
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator, 384u * 1024u) ==
         H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(memory.live == 1u);
  size_t committed_bytes = memory.bytes;
  char large_record[4u * H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES];
  make_record(large_record, sizeof(large_record));
  memory.fail_call =
      memory.calls + 3u; /* roll back two temporary growth pages */
  h2_gizclaw_e2e_ledger_append(&ledger, large_record, sizeof(large_record));
  assert(ledger.error == H2_PAL_ERR_NO_MEMORY && ledger.records == 1u);
  assert(ledger.size == sizeof(line) - 1u && memory.live == 1u);
  assert(memory.bytes == committed_bytes && memory.frees == 2u);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_ERR_NO_MEMORY);
  size_t calls = memory.calls;
  h2_gizclaw_e2e_ledger_append(&ledger, line, sizeof(line) - 1u);
  assert(memory.calls == calls && ledger.records == 1u && !ledger.frozen);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  assert(memory.live == 0u && memory.bytes == 0u && memory.frees == 3u);
}

static void test_invalid_paged_inputs(void) {
  test_memory_t memory = {0};
  h2_pal_mem_api_t allocator = memory_api(&memory);
  h2_pal_mem_api_t invalid = {0};
  h2_gizclaw_e2e_ledger_t ledger;
  assert(h2_gizclaw_e2e_ledger_init_paged(NULL, &allocator, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, NULL, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &invalid, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_ledger_init_paged(&ledger, &allocator, 512u) ==
         H2_PAL_OK);
  h2_gizclaw_e2e_ledger_append(&ledger, NULL, 0u);
  assert(ledger.error == H2_PAL_ERR_INVALID_STATE && memory.calls == 0u);
  h2_gizclaw_e2e_ledger_deinit(&ledger);
  h2_gizclaw_e2e_ledger_deinit(NULL);
}

int main(void) {
  test_borrowed();
  test_cross_page_copy_and_replay();
  test_original_capacity_and_overflow();
  test_allocation_failures();
  test_invalid_paged_inputs();
  return 0;
}
