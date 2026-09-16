#define _POSIX_C_SOURCE 200809L
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct h2_jieli_sdk_sem {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned count;
};
static struct native_task {
    pthread_t thread;
    void (*entry)(void *);
    void *ctx;
    char name[32];
    int deleted;
} native_tasks[2];
static unsigned created;
static const char *expected_policy;
static atomic_int release_second, wrong_delete, allocations;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

void *h2_jieli_sdk_malloc(size_t bytes) {
    void *p = malloc(bytes);
    if (p != NULL) atomic_fetch_add(&allocations, 1);
    return p;
}
void h2_jieli_sdk_free(void *p) {
    if (p != NULL) atomic_fetch_sub(&allocations, 1);
    free(p);
}
h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t count) {
    h2_jieli_sdk_sem_t *s = h2_jieli_sdk_malloc(sizeof(*s));
    assert(s != NULL);
    assert(pthread_mutex_init(&s->mutex, NULL) == 0);
    assert(pthread_cond_init(&s->changed, NULL) == 0);
    s->count = count;
    return s;
}
void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *s) {
    assert(pthread_cond_destroy(&s->changed) == 0);
    assert(pthread_mutex_destroy(&s->mutex) == 0);
    h2_jieli_sdk_free(s);
}
int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *s) {
    assert(pthread_mutex_lock(&s->mutex) == 0);
    ++s->count;
    assert(pthread_cond_signal(&s->changed) == 0);
    assert(pthread_mutex_unlock(&s->mutex) == 0);
    return 0;
}
int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *s, uint32_t timeout) {
    assert(timeout == H2_JIELI_SDK_WAIT_FOREVER);
    assert(pthread_mutex_lock(&s->mutex) == 0);
    while (s->count == 0) assert(pthread_cond_wait(&s->changed, &s->mutex) == 0);
    --s->count;
    assert(pthread_mutex_unlock(&s->mutex) == 0);
    return 0;
}
static void *native_entry(void *user) {
    struct native_task *task = user;
    task->entry(task->ctx);
    return NULL;
}
int h2_jieli_sdk_task_create(void (*entry)(void *), void *ctx,
                           const char *policy_name, const char *name, size_t stack_bytes) {
    assert(strcmp(policy_name, expected_policy) == 0);
    assert(strncmp(name, "#C", 2) != 0);
    (void)stack_bytes;
    assert(created < 2);
    struct native_task *task = &native_tasks[created++];
    task->entry = entry;
    task->ctx = ctx;
    assert(strlen(name) < sizeof(task->name));
    strcpy(task->name, name);
    return pthread_create(&task->thread, NULL, native_entry, task);
}
void h2_jieli_sdk_task_park(void) {}
int h2_jieli_sdk_task_delete(const char *name) {
    /* SDK lookup is name-based. With duplicate labels, its task-list order
     * may select the newer, still-running worker instead of the joined one. */
    for (unsigned n = created; n != 0; --n) {
        struct native_task *task = &native_tasks[n - 1];
        if (task->deleted || strcmp(name, task->name) != 0) continue;
        if (n == 2 && !atomic_load(&release_second)) {
            atomic_store(&wrong_delete, 1);
            return -1;
        }
        assert(pthread_join(task->thread, NULL) == 0);
        task->deleted = 1;
        return 0;
    }
    return -1;
}
static void first_entry(void *user) { (void)user; }
static void second_entry(void *user) {
    (void)user;
    assert(pthread_mutex_lock(&gate) == 0);
    while (!atomic_load(&release_second)) assert(pthread_cond_wait(&changed, &gate) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
}
int main(void) {
    const h2_pal_task_api_t *api = h2_jieli_wl82_platform_task_api();
    const char *labels[] = {"pal/e2e/producer", "#C0pal/e2e/producer"};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        expected_policy = labels[i];
        created = 0;
        memset(native_tasks, 0, sizeof(native_tasks));
        atomic_store(&release_second, 0);
        atomic_store(&wrong_delete, 0);
        const h2_pal_task_options_t options = {.name = labels[i]};
        h2_pal_task_t *first = NULL;
        h2_pal_task_t *second = NULL;
        assert(h2_pal_task_start(api, &options, first_entry, NULL, &first) == 0);
        assert(h2_pal_task_start(api, &options, second_entry, NULL, &second) == 0);
        assert(h2_pal_task_join(api, first) == H2_PAL_OK);
        assert(!atomic_load(&wrong_delete));
        assert(pthread_mutex_lock(&gate) == 0);
        atomic_store(&release_second, 1);
        assert(pthread_cond_broadcast(&changed) == 0);
        assert(pthread_mutex_unlock(&gate) == 0);
        assert(h2_pal_task_join(api, second) == H2_PAL_OK);
        assert(atomic_load(&allocations) == 0);
    }
    return 0;
}
