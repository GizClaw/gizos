#include "../src/h2_mfg_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  size_t limit, allocated;
  unsigned live, opens, closes, removes;
  int corrupt, short_write;
  uint8_t file[512];
} fixture_t;

static void* allocate(void* user, size_t bytes) {
  fixture_t* f = user;
  if (bytes > f->limit) return NULL;
  uint8_t* data = malloc(bytes + 16);
  assert(data != NULL && !f->live);
  memset(data, 0xa5, bytes + 16);
  f->allocated = bytes;
  f->live = 1;
  return data + 8;
}
static void release(void* user, void* pointer) {
  fixture_t* f = user;
  uint8_t* allocation = (uint8_t*)pointer - 8;
  assert(f->live);
  for (unsigned i = 0; i < 8; ++i) {
    assert(allocation[i] == 0xa5);
    assert(allocation[8 + f->allocated + i] == 0xa5);
  }
  free(allocation);
  f->live = 0;
}
static int open_file(void* user, const char* path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t** out) {
  fixture_t* f = user;
  assert(strcmp(path, "/data/.gizos-mfg-probe") == 0);
  assert(mode == H2_PAL_FS_OPEN_READ || mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE);
  ++f->opens;
  *out = (h2_pal_fs_file_t*)f;
  return 0;
}
static int write_file(void* user, h2_pal_fs_file_t* file, const void* data,
                      size_t bytes, size_t* written) {
  fixture_t* f = user;
  assert(file == (h2_pal_fs_file_t*)f && bytes == sizeof(f->file));
  memcpy(f->file, data, bytes);
  *written = bytes - (unsigned)f->short_write;
  return 0;
}
static int read_file(void* user, h2_pal_fs_file_t* file, void* data,
                     size_t bytes, size_t* read) {
  fixture_t* f = user;
  assert(file == (h2_pal_fs_file_t*)f && bytes == sizeof(f->file));
  memcpy(data, f->file, bytes);
  if (f->corrupt) ((uint8_t*)data)[bytes - 1] ^= 1;
  *read = bytes;
  return 0;
}
static int close_file(void* user, h2_pal_fs_file_t* file) {
  fixture_t* f = user;
  assert(file == (h2_pal_fs_file_t*)f);
  ++f->closes;
  return 0;
}
static int remove_file(void* user, const char* path) {
  fixture_t* f = user;
  assert(strcmp(path, "/data/.gizos-mfg-probe") == 0);
  ++f->removes;
  return 0;
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                               .free = release};
static const h2_pal_fs_vtable_t fs_vtable = {.open = open_file,
                                             .write = write_file,
                                             .read = read_file,
                                             .close = close_file,
                                             .remove = remove_file};

static void run(uint32_t requested, size_t limit, int corrupt, int short_write,
                int expected) {
  fixture_t f = {
      .limit = limit, .corrupt = corrupt, .short_write = short_write};
  h2_pal_mem_api_t mem = {.user = &f, .vtable = &mem_vtable};
  h2_pal_fs_api_t fs = {.user = &f, .vtable = &fs_vtable};
  h2_runtime_t rt = {.mem = &mem, .fs = &fs};
  const h2_mfg_step_t step = {"storage", H2_MFG_STORAGE, 1};
  const h2_mfg_profile_t profile = {.board = "test",
                                    .steps = &step,
                                    .total = 1,
                                    .storage_ram_bytes = requested};
  h2_mfg_t m = {.rt = &rt,
                .profile = &profile,
                .model = {.profile = &profile, .summary = {.total = 1}}};
  const char* action = NULL;
  assert(h2_mfg_hardware_tick(&m, 0, &action) == expected);
  assert(m.model.ready == (expected == 0));
  assert(!f.live && f.opens == f.closes);
  if (f.allocated != 0) {
    assert(f.allocated == (requested == 0 ? 65536 : requested));
    assert(f.removes == 1);
  } else {
    assert(f.opens == 0 && f.removes == 0);
  }
}

int main(void) {
  run(0, 65536, 0, 0, 0);
  run(32768, 32768, 0, 0, 0);
  run(0, 32768, 0, 0, H2_PAL_ERR_NO_MEMORY);
  run(32768, 32768, 1, 0, H2_PAL_ERR_IO);
  run(32768, 32768, 0, 1, H2_PAL_ERR_IO);
  run(511, 65536, 0, 0, H2_PAL_ERR_INVALID_ARG);
  run(65537, 65536, 0, 0, H2_PAL_ERR_INVALID_ARG);
  return 0;
}
