"""Seed the real registry at generation boundaries without a production test hook."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / 'native_component_src/jieli/wl82/h2_pal_core'

CASES = r'''
static unsigned old_calls, new_calls, between_calls;
static int new_result;
static h2_pal_system_event_subscription_t *new_sub;
static int generation_new(void *user, const h2_pal_system_event_t *event) {
    (void)user; (void)event; ++new_calls; return H2_PAL_OK;
}
static int generation_old(void *user, const h2_pal_system_event_t *event) {
    (void)user; ++old_calls;
    if (old_calls == 1u)
        new_result = system_event_subscribe(NULL, event->type, generation_new,
                                            NULL, &new_sub);
    return H2_PAL_OK;
}
static int generation_between(void *user, const h2_pal_system_event_t *event) {
    (void)user; (void)event; ++between_calls; return H2_PAL_OK;
}
static h2_atomic_int_t saturated_entered, saturated_done;
static void generation_atomic_init(void) {
#define INIT(name) assert(h2_atomic_int_init(&(name), 0) == H2_ATOMIC_OK)
    INIT(live); INIT(parked); INIT(resume_lock); INIT(park_create);
    INIT(create_parked); INIT(resume_create); INIT(calls);
    INIT(dispatch_entered); INIT(dispatch_exit);
    INIT(unsubscribe_entered); INIT(unsubscribe_done);
    INIT(saturated_entered); INIT(saturated_done);
#undef INIT
}
static void generation_atomic_destroy(void) {
#define DESTROY(name) h2_atomic_int_destroy(&(name))
    DESTROY(live); DESTROY(parked); DESTROY(resume_lock); DESTROY(park_create);
    DESTROY(create_parked); DESTROY(resume_create); DESTROY(calls);
    DESTROY(dispatch_entered); DESTROY(dispatch_exit);
    DESTROY(unsubscribe_entered); DESTROY(unsubscribe_done);
    DESTROY(saturated_entered); DESTROY(saturated_done);
#undef DESTROY
}
static void *saturated_unsubscribe(void *subscription) {
    h2_atomic_store(&saturated_entered, 1);
    system_event_unsubscribe(NULL, subscription);
    h2_atomic_store(&saturated_done, 1);
    return NULL;
}
static void test_saturated(void) {
    assert(system_event_init(NULL) == H2_PAL_OK);
    const h2_pal_system_event_t event = {.type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED};
    h2_pal_system_event_subscription_t *sub = NULL, *replacement = NULL;
    assert(system_event_subscribe(NULL, event.type, generation_new, NULL, &sub) == H2_PAL_OK);
    const uint32_t previous = h2_jieli_atomic_load_u32(&s_lifecycle);
    h2_jieli_atomic_store_u32(&s_lifecycle, (previous & ~EVENT_REFS) | EVENT_REFS);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, saturated_unsubscribe, sub) == 0);
    while (!h2_atomic_load(&saturated_entered)) sched_yield();
    h2_jieli_sdk_sleep_ms(30u);
    assert(!h2_atomic_load(&saturated_done));
    h2_jieli_atomic_store_u32(&s_lifecycle, previous);
    assert(pthread_join(thread, NULL) == 0);
    assert(h2_atomic_load(&saturated_done));
    assert(system_event_post(NULL, &event, 0u) == H2_PAL_OK);
    drain();
    assert(new_calls == 0u);
    assert(system_event_subscribe(NULL, event.type, generation_new, NULL, &replacement) == H2_PAL_OK);
    assert(replacement == sub);
    system_event_unsubscribe(NULL, replacement);
    system_event_deinit(NULL);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    generation_atomic_init();
    if (strcmp(argv[1], "saturated") == 0) {
        test_saturated();
        generation_atomic_destroy();
        return 0;
    }
    const int limit = strcmp(argv[1], "limit") == 0;
    assert(system_event_init(NULL) == H2_PAL_OK);
    event_lock_wait(s_lock);
    /* Assignment fits the original 32-bit type too: before runs stay strict. */
    s_generation = limit ? (__typeof__(s_generation))-2 : UINT32_MAX - 1u;
    (void)h2_jieli_sdk_mutex_unlock(s_lock);
    h2_pal_system_event_subscription_t *old_sub = NULL;
    const h2_pal_system_event_t event = {.type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED};
    assert(system_event_subscribe(NULL, event.type, generation_old, NULL, &old_sub) == H2_PAL_OK);
    pause_driver();
    assert(system_event_post(NULL, &event, 0) == H2_PAL_OK);
    h2_pal_system_event_subscription_t *between = NULL;
    if (!limit)
        assert(system_event_subscribe(NULL, event.type, generation_between, NULL, &between) == H2_PAL_OK);
    assert(old_calls == 0u && new_calls == 0u && between_calls == 0u);
    resume_driver();
    drain();
    if (limit) {
        assert(new_result == H2_PAL_ERR_FULL && new_sub == NULL);
    } else {
        assert(new_result == H2_PAL_OK && new_sub != NULL);
    }
    assert(old_calls == 1u && new_calls == 0u && between_calls == 0u);
    for (unsigned i = 0; i < 3; ++i)
        assert(system_event_post(NULL, &event, 0) == H2_PAL_OK);
    drain();
    assert(old_calls == 4u && new_calls == (limit ? 0u : 3u));
    assert(between_calls == (limit ? 0u : 3u));
    system_event_unsubscribe(NULL, between);
    system_event_unsubscribe(NULL, old_sub);
    system_event_unsubscribe(NULL, new_sub);
    if (limit) { /* Empty slots cannot bypass generation exhaustion. */
        assert(system_event_subscribe(NULL, event.type, generation_new, NULL, &new_sub) == H2_PAL_ERR_FULL);
        assert(new_sub == NULL);
    }
    system_event_deinit(NULL);
    assert(system_event_init(NULL) == H2_PAL_OK);
    assert(system_event_subscribe(NULL, event.type, generation_new, NULL, &new_sub) == H2_PAL_OK);
    system_event_unsubscribe(NULL, new_sub);
    system_event_deinit(NULL);
    generation_atomic_destroy();
    return thread_cases_main(); /* Retain owner, quiescence and self-unsubscribe coverage. */
}
'''


class GenerationTest(unittest.TestCase):
    def test_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'generation.c'
            binary = Path(directory) / 'generation'
            unit.write_text(
                f'#include "{CORE / "src/h2_jieli_wl82_platform_system_event.c"}"\n'
                '#define main thread_cases_main\n'
                f'#include "{CORE / "tests/src/test_jieli_wl82_event_threads.c"}"\n'
                '#undef main\n' + CASES)
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')),
                            '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                            '-I', str(ROOT / 'libs/pal/include'),
                            '-I', str(ROOT / 'libs/atomic/include'),
                            '-I', str(CORE / 'include'), str(unit),
                            str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'),
                            '-o', str(binary)], check=True)
            for case in ('snapshot', 'limit', 'saturated'):
                with self.subTest(case=case):
                    subprocess.run([str(binary), case], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
