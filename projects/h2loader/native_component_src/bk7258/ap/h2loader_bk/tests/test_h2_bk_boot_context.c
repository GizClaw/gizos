#include "h2_bk_boot_context.h"
#include "h2_bk_h2loader.h"
#include "h2_loader_metadata.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SHA256 \
  "abababababababababababababababababababababababababababababababab"

/* Persisted Partition 2 record, with Stage held separately so a test can prove
 * the adapter never treats an uncommitted Stage role as the candidate role. */
typedef struct pref_fixture {
  h2_pal_pref_namespace_t ns;
  uint8_t partition_1[1024], partition_2[1024], stage[1024];
  size_t partition_1_len, partition_2_len, stage_len;
  int open_result;
  unsigned opens;
} pref_fixture_t;

static void* test_alloc(void* user, size_t len) {
  (void)user;
  return malloc(len);
}
static void test_free(void* user, void* ptr) {
  (void)user;
  free(ptr);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = test_alloc,
                                               .free = test_free};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static const h2_pal_mem_api_t other_mem = {.vtable = &mem_vtable};

static int pref_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return H2_PAL_OK;
}
static int pref_get_blob(h2_pal_pref_namespace_t* ns,
                         const h2_pal_mem_api_t* allocator, const char* key,
                         void** out_data, size_t* out_len) {
  pref_fixture_t* fixture = ns->user;
  const uint8_t* data = fixture->partition_2;
  size_t len = fixture->partition_2_len;
  if (strcmp(key, "stage") == 0) {
    data = fixture->stage;
    len = fixture->stage_len;
  } else if (strcmp(key, "partition_1") == 0) {
    data = fixture->partition_1;
    len = fixture->partition_1_len;
  } else {
    assert(strcmp(key, "partition_2") == 0);
  }
  if (len == 0u) return H2_PAL_ERR_NOT_FOUND;
  *out_data = h2_pal_mem_alloc(allocator, len);
  assert(*out_data != NULL);
  memcpy(*out_data, data, len);
  *out_len = len;
  return H2_PAL_OK;
}
static int pref_open(void* user, const char* name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out_namespace) {
  pref_fixture_t* fixture = user;
  (void)mode;
  assert(strcmp(name_space, H2_LOADER_PREF_NAMESPACE) == 0);
  ++fixture->opens;
  if (fixture->open_result != H2_PAL_OK) return fixture->open_result;
  fixture->ns = (h2_pal_pref_namespace_t){
      .user = fixture, .close = pref_close, .get_blob = pref_get_blob};
  *out_namespace = &fixture->ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};

static h2_loader_metadata_t record(h2_loader_image_role_t role, uint64_t size) {
  h2_loader_metadata_t metadata = {
      .valid = 1, .package_size = 1024u, .image_size = size, .role = role};
  (void)snprintf(metadata.package_checksum, sizeof(metadata.package_checksum),
                 "%s", IMAGE_SHA256);
  (void)snprintf(metadata.image_checksum, sizeof(metadata.image_checksum), "%s",
                 IMAGE_SHA256);
  (void)snprintf(metadata.version, sizeof(metadata.version), "%s", "1.0.0");
  (void)snprintf(metadata.board, sizeof(metadata.board), "%s", "board");
  (void)snprintf(metadata.target, sizeof(metadata.target), "%s", "bk7258");
  return metadata;
}

static void commit_partition_2(pref_fixture_t* fixture,
                               const h2_loader_metadata_t* metadata) {
  assert(h2_loader_metadata_encode(H2_LOADER_METADATA_SLOT_PARTITION_2,
                                   metadata, fixture->partition_2,
                                   sizeof(fixture->partition_2),
                                   &fixture->partition_2_len) == H2_PAL_OK);
}

static void commit_partition_1(pref_fixture_t* fixture,
                               const h2_loader_metadata_t* metadata) {
  assert(h2_loader_metadata_encode(H2_LOADER_METADATA_SLOT_PARTITION_1,
                                   metadata, fixture->partition_1,
                                   sizeof(fixture->partition_1),
                                   &fixture->partition_1_len) == H2_PAL_OK);
}

int main(void) {
  pref_fixture_t fixture = {0}, other_fixture = {0};
  const h2_pal_pref_api_t pref = {.user = &fixture, .vtable = &pref_vtable};
  const h2_pal_pref_api_t other_pref = {.user = &other_fixture,
                                        .vtable = &pref_vtable};
  const h2_pal_pref_api_t no_vtable = {0};
  const h2_pal_mem_api_t no_mem_vtable = {0};
  uint64_t size = 99u;

  /* Unbound: fail closed, without touching a Preference backend. */
  assert(h2_bk_boot_candidate_is_loader(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);
  assert(size == 0u && fixture.opens == 0u);
  assert(h2_bk_boot_context_ready() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_boot_loader_window_committed() == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.opens == 0u);

  assert(h2_bk_h2loader_boot_context_bind(NULL, &mem) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_h2loader_boot_context_bind(&pref, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_h2loader_boot_context_bind(&no_vtable, &mem) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_h2loader_boot_context_bind(&pref, &no_mem_vtable) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);

  /* Duplicate binding is idempotent only for the same borrowed pair. */
  assert(h2_bk_h2loader_boot_context_bind(&pref, &mem) == H2_PAL_OK);
  assert(h2_bk_h2loader_boot_context_bind(&pref, &mem) == H2_PAL_OK);
  assert(h2_bk_h2loader_boot_context_bind(&other_pref, &mem) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_h2loader_boot_context_bind(&pref, &other_mem) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_boot_context_ready() == H2_PAL_OK);

  /* Partition 1 reads current state: copy-back clears it before erasing A. */
  assert(h2_bk_boot_loader_window_committed() == 0);
  const h2_loader_metadata_t cleared = {0};
  const h2_loader_metadata_t active =
      record(H2_LOADER_IMAGE_ROLE_H2LOADER, 2380u * 1024u);
  commit_partition_1(&fixture, &active);
  assert(h2_bk_boot_loader_window_committed() == 1);
  commit_partition_1(&fixture, &cleared);
  assert(h2_bk_boot_loader_window_committed() == 0);
  commit_partition_1(&fixture, &active);
  fixture.partition_1[0] ^= 0xffu;
  assert(h2_bk_boot_loader_window_committed() == H2_PAL_ERR_FORMAT);
  fixture.partition_1[0] ^= 0xffu;
  fixture.open_result = H2_PAL_ERR_IO;
  assert(h2_bk_boot_loader_window_committed() == H2_PAL_ERR_IO);
  fixture.open_result = H2_PAL_OK;

  /* Missing, cleared and Stage-only records are not candidates. */
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);
  const h2_loader_metadata_t stage =
      record(H2_LOADER_IMAGE_ROLE_H2LOADER, 68u * 1024u);
  assert(h2_loader_metadata_encode(H2_LOADER_METADATA_SLOT_STAGE, &stage,
                                   fixture.stage, sizeof(fixture.stage),
                                   &fixture.stage_len) == H2_PAL_OK);
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);
  const h2_loader_metadata_t invalid = {0};
  commit_partition_2(&fixture, &invalid);
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);

  /* Each call reads current committed state, so a role change is not cached. */
  h2_loader_metadata_t loader =
      record(H2_LOADER_IMAGE_ROLE_H2LOADER, 2380u * 1024u);
  commit_partition_2(&fixture, &loader);
  assert(h2_bk_boot_candidate_is_loader(&size) == 1);
  assert(size == 2380u * 1024u);
  h2_loader_metadata_t app = record(H2_LOADER_IMAGE_ROLE_APP, 5100u * 1024u);
  commit_partition_2(&fixture, &app);
  assert(h2_bk_boot_candidate_is_loader(&size) == 0);
  assert(size == 5100u * 1024u);

  /* Malformed storage and backend failures propagate rather than defaulting. */
  fixture.partition_2[0] ^= 0xffu;
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_FORMAT);
  assert(size == 0u);
  fixture.partition_2[0] ^= 0xffu;
  fixture.open_result = H2_PAL_ERR_IO;
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_IO);
  fixture.open_result = H2_PAL_OK;

  /* Unbind checks ownership, is repeatable, and allows a fresh Runtime. */
  assert(h2_bk_h2loader_boot_context_unbind(NULL, &mem) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_h2loader_boot_context_unbind(&other_pref, &mem) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_boot_candidate_is_loader(&size) == 0);
  assert(h2_bk_h2loader_boot_context_unbind(&pref, &mem) == H2_PAL_OK);
  assert(h2_bk_h2loader_boot_context_unbind(&pref, &mem) == H2_PAL_OK);
  assert(h2_bk_boot_candidate_is_loader(&size) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_h2loader_boot_context_bind(&other_pref, &other_mem) ==
         H2_PAL_OK);
  assert(h2_bk_h2loader_boot_context_unbind(&other_pref, &other_mem) ==
         H2_PAL_OK);
  return 0;
}
