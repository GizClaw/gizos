"""Exercise real JieLi sync/queue allocator ownership with SDK fault fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / "native_component_src/jieli/wl82/h2_pal_core"
FIXTURE = r'''#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port_fake.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct owner { unsigned calls, frees, live, fail_at; };
union block {
    max_align_t alignment;
    struct { struct owner *owner; } metadata;
};
static void *allocate(void *user, size_t bytes) {
    struct owner *owner = user;
    ++owner->calls;
    if (owner->calls == owner->fail_at) return NULL;
    union block *block = malloc(sizeof(*block) + bytes);
    assert(block != NULL);
    block->metadata.owner = owner;
    ++owner->live;
    memset(block + 1, 0xa5, bytes);
    return block + 1;
}
static void release(void *user, void *pointer) {
    struct owner *owner = user;
    assert(pointer != NULL);
    union block *block = (union block *)pointer - 1;
    assert(block->metadata.owner == owner);
    assert(owner->live > 0);
    --owner->live;
    ++owner->frees;
    free(block);
}
static const h2_pal_mem_vtable_t memory = {.alloc = allocate, .free = release};
static int create(unsigned kind, const h2_pal_mem_api_t *allocator, void **out) {
    const h2_pal_sync_api_t *sync = h2_jieli_wl82_platform_sync_api();
    switch (kind) {
    case 0: {
        const h2_pal_mutex_config_t config = {.allocator = allocator};
        h2_pal_mutex_t *value = NULL;
        int result = h2_pal_mutex_create(sync, &config, &value);
        *out = value;
        return result;
    }
    case 1: {
        const h2_pal_semaphore_config_t config = {.allocator = allocator, .max_count = 1};
        h2_pal_semaphore_t *value = NULL;
        int result = h2_pal_semaphore_create(sync, &config, &value);
        *out = value;
        return result;
    }
    case 2: {
        const h2_pal_cond_config_t config = {.allocator = allocator};
        h2_pal_cond_t *value = NULL;
        int result = h2_pal_cond_create(sync, &config, &value);
        *out = value;
        return result;
    }
    default: {
        const h2_pal_queue_config_t config = {.allocator = allocator, .item_size = sizeof(int), .item_count = 2};
        h2_pal_queue_t *value = NULL;
        int result = h2_pal_queue_create(h2_jieli_wl82_platform_queue_api(), &config, &value);
        *out = value;
        return result;
    }
    }
}
static void destroy(unsigned kind, void *value) {
    const h2_pal_sync_api_t *sync = h2_jieli_wl82_platform_sync_api();
    if (kind == 0) {
        assert(h2_pal_mutex_lock(sync, value) == H2_PAL_OK);
        assert(h2_pal_mutex_unlock(sync, value) == H2_PAL_OK);
        assert(h2_pal_mutex_destroy(sync, value) == H2_PAL_OK);
    } else if (kind == 1) {
        assert(h2_pal_semaphore_give(sync, value) == H2_PAL_OK);
        assert(h2_pal_semaphore_take(sync, value, 0) == H2_PAL_OK);
        assert(h2_pal_semaphore_destroy(sync, value) == H2_PAL_OK);
    } else if (kind == 2) {
        assert(h2_pal_cond_signal(sync, value) == H2_PAL_OK);
        assert(h2_pal_cond_destroy(sync, value) == H2_PAL_OK);
    } else {
        const h2_pal_queue_api_t *queue = h2_jieli_wl82_platform_queue_api();
        int sent = 123, received = 0;
        assert(h2_pal_queue_send(queue, value, &sent, 0) == H2_PAL_OK);
        assert(h2_pal_queue_recv(queue, value, &received, 0) == H2_PAL_OK);
        assert(received == sent);
        h2_pal_queue_destroy(queue, value);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned kind = (unsigned)atoi(argv[1]);
    assert(kind < 4);
    h2_jieli_fake_reset();
    struct owner a = {0}, b = {0};
    const h2_pal_mem_api_t first = {.user = &a, .vtable = &memory};
    const h2_pal_mem_api_t second = {.user = &b, .vtable = &memory};
    void *one = NULL, *two = NULL;
    assert(create(kind, &first, &one) == H2_PAL_OK);
    assert(a.calls > 0 && a.live > 0);
    assert(create(kind, &second, &two) == H2_PAL_OK);
    assert(b.calls > 0 && b.live > 0);
    unsigned allocations = a.calls;
    destroy(kind, two);
    assert(b.live == 0 && b.calls == b.frees && a.live > 0);
    destroy(kind, one);
    assert(a.live == 0 && a.calls == a.frees);
    assert(h2_jieli_fake_live_allocations() == 0);
    for (unsigned n = 1; n <= allocations; ++n) {
        a = (struct owner){.fail_at = n};
        one = (void *)1;
        assert(create(kind, &first, &one) == H2_PAL_ERR_NO_MEMORY);
        assert(one == NULL && a.live == 0 && a.frees + 1 == a.calls);
        assert(h2_jieli_fake_live_allocations() == 0);
    }
    if (kind != 2) {
        a = (struct owner){0};
        h2_jieli_fake_fail_next_malloc();
        assert(create(kind, &first, &one) == H2_PAL_ERR_NO_MEMORY);
        assert(one == NULL && a.live == 0 && a.calls == a.frees);
        assert(h2_jieli_fake_live_allocations() == 0);
    }
    assert(create(kind, NULL, &one) == H2_PAL_OK);
    destroy(kind, one);
    assert(h2_jieli_fake_live_allocations() == 0);
    return 0;
}
'''

class AllocatorContract(unittest.TestCase):
    def test_all_create_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "fixture.c"
            binary = Path(directory) / "fixture"
            source.write_text(FIXTURE)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "libs/pal/include"),
                "-I" + str(CORE / "include"),
                "-I" + str(CORE / "tests/include"),
                str(source), str(CORE / "src/h2_jieli_wl82_platform_sync.c"),
                str(CORE / "src/h2_jieli_wl82_platform_queue.c"),
                str(CORE / "tests/src/h2_jieli_wl82_sdk_port_fake.c"),
                "-o", str(binary),
            ], check=True)
            for kind, name in enumerate(("mutex", "semaphore", "condition", "queue")):
                with self.subTest(path=name):
                    subprocess.run([str(binary), str(kind)], check=True, timeout=20)

if __name__ == "__main__":
    unittest.main()
