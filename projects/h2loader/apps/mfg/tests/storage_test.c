#include "../src/h2_mfg_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct heap {
  unsigned allocations, frees;
  int fail;
  void* owned;
} heap_t;

static void* allocate(void* user, size_t size) {
  heap_t* heap = user;
  assert(size == 65536u && heap->owned == NULL);
  ++heap->allocations;
  if (heap->fail) return NULL;
  heap->owned = malloc(size);
  assert(heap->owned);
  return heap->owned;
}

static void release(void* user, void* pointer) {
  heap_t* heap = user;
  assert(pointer != NULL && pointer == heap->owned);
  ++heap->frees;
  free(pointer);
  heap->owned = NULL;
}

struct h2_pal_fs_file {
  unsigned unused;
};

typedef struct filesystem {
  struct h2_pal_fs_file file;
  uint8_t data[512];
  unsigned opens, writes, reads, closes, removes;
  int fail_write, corrupt_read;
} filesystem_t;

static int open_file(void* user, const char* path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t** out) {
  filesystem_t* fs = user;
  assert(strcmp(path, "/data/.gizos-mfg-probe") == 0);
  assert(mode == (fs->opens == 0 ? H2_PAL_FS_OPEN_WRITE_TRUNCATE
                                 : H2_PAL_FS_OPEN_READ));
  ++fs->opens;
  *out = &fs->file;
  return H2_PAL_OK;
}

static int write_file(void* user, h2_pal_fs_file_t* file, const void* data,
                      size_t size, size_t* written) {
  filesystem_t* fs = user;
  assert(file == &fs->file && size == sizeof(fs->data));
  const uint8_t* bytes = data;
  for (size_t i = 0; i < size; ++i)
    assert(bytes[i] == (uint8_t)(i ^ (i >> 8) ^ 0xa5));
  ++fs->writes;
  *written = 0;
  if (fs->fail_write) return H2_PAL_ERR_IO;
  memcpy(fs->data, data, size);
  *written = size;
  return H2_PAL_OK;
}

static int read_file(void* user, h2_pal_fs_file_t* file, void* data,
                     size_t size, size_t* read) {
  filesystem_t* fs = user;
  assert(file == &fs->file && size == sizeof(fs->data));
  ++fs->reads;
  memcpy(data, fs->data, size);
  if (fs->corrupt_read) ((uint8_t*)data)[size - 1u] ^= 1u;
  *read = size;
  return H2_PAL_OK;
}

static int close_file(void* user, h2_pal_fs_file_t* file) {
  filesystem_t* fs = user;
  assert(file == &fs->file);
  ++fs->closes;
  return H2_PAL_OK;
}

static int remove_file(void* user, const char* path) {
  filesystem_t* fs = user;
  assert(strcmp(path, "/data/.gizos-mfg-probe") == 0);
  ++fs->removes;
  return H2_PAL_OK;
}

int main(void) {
  const h2_pal_mem_vtable_t memory = {.alloc = allocate, .free = release};
  const h2_pal_fs_vtable_t files = {.open = open_file,
                                    .write = write_file,
                                    .read = read_file,
                                    .close = close_file,
                                    .remove = remove_file};
  const h2_mfg_step_t step = {"storage", H2_MFG_STORAGE, 1};
  const h2_mfg_profile_t profile = {.steps = &step, .total = 1};
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    heap_t heap = {.fail = scenario == 1};
    filesystem_t fs = {.fail_write = scenario == 2,
                       .corrupt_read = scenario == 3};
    const h2_pal_mem_api_t mem_api = {.user = &heap, .vtable = &memory};
    const h2_pal_fs_api_t fs_api = {.user = &fs, .vtable = &files};
    h2_runtime_t runtime = {.mem = &mem_api, .fs = &fs_api};
    h2_mfg_t m = {.rt = &runtime,
                  .profile = &profile,
                  .model = {.profile = &profile, .summary = {.total = 1}}};
    const char* action = NULL;
    int rc = h2_mfg_hardware_tick(&m, 100, &action);
    if (scenario == 1) {
      assert(rc == H2_PAL_ERR_NO_MEMORY && fs.opens == 0);
    } else if (scenario >= 2) {
      assert(rc == H2_PAL_ERR_IO && fs.removes == 1);
      assert(fs.closes == (scenario == 2 ? 1u : 2u));
    } else {
      assert(rc == H2_PAL_OK && m.model.ready);
      assert(action && strcmp(action, "confirm") == 0);
      assert(fs.writes == 1 && fs.reads == 1 && fs.closes == 2 &&
             fs.removes == 1);
    }
    if (rc != H2_PAL_OK) assert(!m.model.ready && action == NULL);
    assert(heap.allocations == 1u);
    assert(heap.frees == (scenario == 1 ? 0u : 1u));
    assert(heap.owned == NULL);
  }
  return 0;
}
