#include "ledger.h"
#include <string.h>

struct h2_gizclaw_e2e_ledger_page {
  h2_gizclaw_e2e_ledger_page_t *next;
  size_t used;
  char data[H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES];
};

static void free_pages(const h2_pal_mem_api_t *allocator,
                       h2_gizclaw_e2e_ledger_page_t *page) {
  while (page != NULL) {
    h2_gizclaw_e2e_ledger_page_t *next = page->next;
    h2_pal_mem_free(allocator, page);
    page = next;
  }
}

int h2_gizclaw_e2e_ledger_init(h2_gizclaw_e2e_ledger_t *ledger, char *buffer,
                              size_t capacity) {
  if (ledger == NULL || buffer == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  *ledger = (h2_gizclaw_e2e_ledger_t){.data = buffer, .capacity = capacity};
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_ledger_init_paged(h2_gizclaw_e2e_ledger_t *ledger,
                                     const h2_pal_mem_api_t *allocator,
                                     size_t capacity) {
  if (ledger == NULL || allocator == NULL || allocator->vtable == NULL ||
      allocator->vtable->alloc == NULL || allocator->vtable->free == NULL ||
      capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  *ledger =
      (h2_gizclaw_e2e_ledger_t){.capacity = capacity, .allocator = allocator};
  return H2_PAL_OK;
}

static int append_paged(h2_gizclaw_e2e_ledger_t *ledger, const char *record,
                        size_t size) {
  size_t available = ledger->last == NULL
                         ? 0u
                         : sizeof(ledger->last->data) - ledger->last->used;
  size_t needed = size > available ? size - available : 0u;
  h2_gizclaw_e2e_ledger_page_t *first = NULL, *last = NULL;
  /* Reserve the whole record before changing any committed page or counter. */
  while (needed != 0u) {
    h2_gizclaw_e2e_ledger_page_t *page =
        h2_pal_mem_alloc(ledger->allocator, sizeof(*page));
    if (page == NULL) {
      free_pages(ledger->allocator, first);
      return H2_PAL_ERR_NO_MEMORY;
    }
    page->next = NULL;
    page->used = 0u;
    if (last == NULL)
      first = page;
    else
      last->next = page;
    last = page;
    needed -= needed < sizeof(page->data) ? needed : sizeof(page->data);
  }
  h2_gizclaw_e2e_ledger_page_t *page = ledger->last;
  if (page == NULL) {
    ledger->first = first;
    page = first;
  } else {
    page->next = first;
    if (page->used == sizeof(page->data))
      page = first;
  }
  while (size != 0u) {
    size_t count = sizeof(page->data) - page->used;
    if (count > size)
      count = size;
    memcpy(page->data + page->used, record, count);
    page->used += count;
    record += count;
    size -= count;
    if (size != 0u)
      page = page->next;
  }
  ledger->last = page;
  return H2_PAL_OK;
}

void h2_gizclaw_e2e_ledger_append(h2_gizclaw_e2e_ledger_t *ledger,
                                 const char *record, size_t size) {
  if (ledger == NULL || ledger->error != H2_PAL_OK)
    return;
  static const char prefix[] = "H2_GIZCLAW_E2E ";
  if (ledger->frozen || (ledger->data == NULL && ledger->allocator == NULL) ||
      record == NULL || size < sizeof(prefix) || record[size - 1u] != '\n' ||
      memcmp(record, prefix, sizeof(prefix) - 1u) != 0 ||
      memchr(record, '\0', size) != NULL ||
      memchr(record, '\n', size - 1u) != NULL) {
    ledger->error = H2_PAL_ERR_INVALID_STATE;
    return;
  }
  if (ledger->size > ledger->capacity ||
      size > ledger->capacity - ledger->size) {
    ledger->error = H2_PAL_ERR_NO_SPACE;
    return;
  }
  if (ledger->allocator != NULL) {
    ledger->error = append_paged(ledger, record, size);
    if (ledger->error != H2_PAL_OK)
      return;
  } else {
    memcpy(ledger->data + ledger->size, record, size);
  }
  ledger->size += size;
  ledger->records++;
}

static uint32_t update_crc(uint32_t crc, const char *data, size_t size) {
  for (size_t i = 0u; i < size; ++i) {
    crc ^= (uint8_t)data[i];
    for (unsigned bit = 0u; bit < 8u; ++bit)
      crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}

int h2_gizclaw_e2e_ledger_freeze(h2_gizclaw_e2e_ledger_t *ledger) {
  if (ledger == NULL || (ledger->data == NULL && ledger->allocator == NULL) ||
      ledger->frozen)
    return H2_PAL_ERR_INVALID_STATE;
  if (ledger->error != H2_PAL_OK)
    return ledger->error;
  if (ledger->size == 0u)
    return H2_PAL_ERR_INVALID_STATE;
  /* IEEE CRC32 detects lost/truncated UART records; it is not an authenticity
   * claim. The host receipt additionally hashes the exact complete log. */
  uint32_t crc = UINT32_MAX;
  if (ledger->allocator != NULL) {
    for (const h2_gizclaw_e2e_ledger_page_t *page = ledger->first; page != NULL;
         page = page->next)
      crc = update_crc(crc, page->data, page->used);
  } else {
    crc = update_crc(crc, ledger->data, ledger->size);
  }
  ledger->crc32 = ~crc;
  ledger->frozen = true;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_ledger_span(const h2_gizclaw_e2e_ledger_t *ledger,
                               size_t offset, const char **out_data,
                               size_t *out_size) {
  if (out_data != NULL)
    *out_data = NULL;
  if (out_size != NULL)
    *out_size = 0u;
  if (ledger == NULL || out_data == NULL || out_size == NULL ||
      offset > ledger->size)
    return H2_PAL_ERR_INVALID_ARG;
  if (!ledger->frozen)
    return H2_PAL_ERR_INVALID_STATE;
  if (offset == ledger->size)
    return H2_PAL_OK;
  if (ledger->allocator == NULL) {
    *out_data = ledger->data + offset;
    *out_size = ledger->size - offset;
    return H2_PAL_OK;
  }
  const h2_gizclaw_e2e_ledger_page_t *page = ledger->first;
  while (page != NULL && offset >= page->used) {
    offset -= page->used;
    page = page->next;
  }
  if (page == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  *out_data = page->data + offset;
  *out_size = page->used - offset;
  return H2_PAL_OK;
}

void h2_gizclaw_e2e_ledger_deinit(h2_gizclaw_e2e_ledger_t *ledger) {
  if (ledger == NULL)
    return;
  free_pages(ledger->allocator, ledger->first);
  *ledger = (h2_gizclaw_e2e_ledger_t){0};
}
