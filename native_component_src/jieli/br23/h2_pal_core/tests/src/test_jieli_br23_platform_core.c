#include "h2_jieli_br23_platform_core.h"
#include "h2_jieli_br23_sdk_port_fake.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            failures++;                                                          \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expression);          \
        }                                                                        \
    } while (0)

static void test_mem_round_trip(void)
{
    const h2_pal_mem_api_t *mem = h2_jieli_br23_platform_mem_api();
    void *block;
    h2_jieli_fake_reset();
    CHECK(h2_pal_mem_alloc(mem, 0u) == NULL);
    block = h2_pal_mem_alloc(mem, 32u);
    CHECK(block != NULL);
    block = h2_pal_mem_realloc(mem, block, 64u);
    CHECK(block != NULL);
    h2_pal_mem_free(mem, block);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void test_log_formats_level_scope_and_crlf(void)
{
    const h2_pal_log_api_t *log = h2_jieli_br23_platform_log_api();
    h2_jieli_fake_reset();
    CHECK(h2_pal_log_write(log, H2_PAL_LOG_INFO, "log", "Hello World") == H2_PAL_OK);
    CHECK(strcmp(h2_jieli_fake_log_output(), "[I][log] Hello World\r\n") == 0);
    h2_jieli_fake_reset();
    CHECK(h2_pal_log_write(log, H2_PAL_LOG_ERROR, NULL, "boom") == H2_PAL_OK);
    CHECK(strcmp(h2_jieli_fake_log_output(), "[E] boom\r\n") == 0);
    CHECK(h2_pal_log_write(log, H2_PAL_LOG_WARN, "s", NULL) == H2_PAL_ERR_INVALID_ARG);
}

static void test_log_truncates_long_messages_without_overflow(void)
{
    const h2_pal_log_api_t *log = h2_jieli_br23_platform_log_api();
    char message[H2_PAL_LOG_MESSAGE_MAX + 200u];
    size_t length;
    memset(message, 'x', sizeof(message) - 1u);
    message[sizeof(message) - 1u] = '\0';
    h2_jieli_fake_reset();
    CHECK(h2_pal_log_write(log, H2_PAL_LOG_DEBUG, "scope", message) == H2_PAL_OK);
    length = h2_jieli_fake_log_length();
    CHECK(length <= H2_JIELI_BR23_LOG_LINE_MAX);
    CHECK(length >= 2u);
    CHECK(h2_jieli_fake_log_output()[length - 2u] == '\r');
    CHECK(h2_jieli_fake_log_output()[length - 1u] == '\n');
}

static void test_log_bounds_long_scope_and_message_together(void)
{
    const h2_pal_log_api_t *log = h2_jieli_br23_platform_log_api();
    char scope[H2_JIELI_BR23_LOG_LINE_MAX];
    char message[H2_PAL_LOG_MESSAGE_MAX];
    size_t length;
    memset(scope, 's', sizeof(scope) - 1u);
    scope[sizeof(scope) - 1u] = '\0';
    memset(message, 'm', sizeof(message) - 1u);
    message[sizeof(message) - 1u] = '\0';
    h2_jieli_fake_reset();
    CHECK(h2_pal_log_write(log, H2_PAL_LOG_INFO, scope, message) == H2_PAL_OK);
    length = h2_jieli_fake_log_length();
    CHECK(length <= H2_JIELI_BR23_LOG_LINE_MAX);
    CHECK(length >= 2u);
    CHECK(h2_jieli_fake_log_output()[length - 2u] == '\r');
    CHECK(h2_jieli_fake_log_output()[length - 1u] == '\n');
    CHECK(strncmp(h2_jieli_fake_log_output(), "[I][sss", 7u) == 0);
}

static void test_time_extends_32bit_wrap_and_sleeps(void)
{
    const h2_pal_time_api_t *time = h2_jieli_br23_platform_time_api();
    uint64_t ms = 0u;
    uint64_t us = 0u;
    h2_pal_time_wall_status_t status;
    h2_jieli_fake_reset();
    h2_jieli_fake_set_time_ms(0xfffffff0u);
    CHECK(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
    CHECK(ms == 0xfffffff0u);
    h2_jieli_fake_set_time_ms(0x10u);
    CHECK(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
    CHECK(ms == 0x100000010ull);
    CHECK(h2_pal_time_get_monotonic_us(time, &us) == H2_PAL_OK);
    CHECK(us == 0x100000010ull * 1000u);
    CHECK(h2_pal_time_get_wall_ms(time, &ms) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(h2_pal_time_get_wall_status(time, &status) == H2_PAL_OK);
    CHECK(status.valid == 0u);
    CHECK(h2_pal_time_sleep_ms(time, 25u) == H2_PAL_OK);
    CHECK(h2_jieli_fake_sleep_total_ms() == 25u);
}

static void test_sync_mutex_and_semaphore(void)
{
    const h2_pal_sync_api_t *sync = h2_jieli_br23_platform_sync_api();
    const h2_pal_mutex_config_t mutex_config = {.name = "m", .flags = 0u};
    const h2_pal_mutex_config_t recursive_config = {.name = "r", .flags = H2_PAL_MUTEX_FLAG_RECURSIVE};
    const h2_pal_semaphore_config_t sem_config = {.name = "s", .initial_count = 1u, .max_count = 2u};
    h2_pal_mutex_t *mutex = NULL;
    h2_pal_semaphore_t *sem = NULL;
    h2_pal_cond_t *cond = NULL;
    const h2_pal_cond_config_t cond_config = {.name = "c"};
    h2_jieli_fake_reset();
    CHECK(h2_pal_mutex_create(sync, &recursive_config, &mutex) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(h2_pal_mutex_create(sync, &mutex_config, &mutex) == H2_PAL_OK);
    CHECK(h2_pal_mutex_lock(sync, mutex) == H2_PAL_OK);
    CHECK(h2_pal_mutex_try_lock(sync, mutex) == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
    CHECK(h2_pal_mutex_try_lock(sync, mutex) == H2_PAL_OK);
    CHECK(h2_pal_mutex_unlock(sync, mutex) == H2_PAL_OK);
    CHECK(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);

    CHECK(h2_pal_semaphore_create(sync, &sem_config, &sem) == H2_PAL_OK);
    CHECK(h2_pal_semaphore_take(sync, sem, 0u) == H2_PAL_OK);
    CHECK(h2_pal_semaphore_take(sync, sem, 30u) == H2_PAL_ERR_TIMEOUT);
    CHECK(h2_pal_semaphore_give(sync, sem) == H2_PAL_OK);
    CHECK(h2_pal_semaphore_give(sync, sem) == H2_PAL_OK);
    /* max_count is 2: a third give must not overshoot the ceiling. */
    CHECK(h2_pal_semaphore_give(sync, sem) == H2_PAL_ERR_FULL);
    CHECK(h2_pal_semaphore_take(sync, sem, 0u) == H2_PAL_OK);
    CHECK(h2_pal_semaphore_take(sync, sem, 0u) == H2_PAL_OK);
    CHECK(h2_pal_semaphore_take(sync, sem, 0u) == H2_PAL_ERR_TIMEOUT);
    CHECK(h2_pal_semaphore_destroy(sync, sem) == H2_PAL_OK);

    CHECK(h2_pal_cond_create(sync, &cond_config, &cond) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void test_queue_fifo_full_timeout_latest_and_close(void)
{
    const h2_pal_queue_api_t *api = h2_jieli_br23_platform_queue_api();
    const h2_pal_queue_config_t config = {.name = "q", .item_size = sizeof(int), .item_count = 2u};
    h2_pal_queue_t *queue = NULL;
    int value;
    int out = 0;
    h2_jieli_fake_reset();
    CHECK(h2_pal_queue_create(api, &config, &queue) == H2_PAL_OK);
    value = 1;
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_OK);
    value = 2;
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_OK);
    value = 3;
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_ERR_FULL);
    CHECK(h2_pal_queue_send(api, queue, &value, 15u) == H2_PAL_ERR_TIMEOUT);
    /* send_latest coalesces onto the newest pending item when full. */
    value = 9;
    CHECK(h2_pal_queue_send_latest(api, queue, &value) == H2_PAL_OK);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_OK);
    CHECK(out == 1);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_OK);
    CHECK(out == 9);
    CHECK(h2_pal_queue_recv(api, queue, &out, 20u) == H2_PAL_ERR_TIMEOUT);
    /* Wrap-around keeps FIFO order. */
    value = 4;
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_OK);
    value = 5;
    CHECK(h2_pal_queue_send_latest(api, queue, &value) == H2_PAL_OK);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_OK);
    CHECK(out == 4);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_OK);
    CHECK(out == 5);
    value = 6;
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_OK);
    CHECK(h2_pal_queue_reset(api, queue) == H2_PAL_OK);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_ERR_TIMEOUT);
    CHECK(h2_pal_queue_close(api, queue) == H2_PAL_OK);
    CHECK(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_ERR_CLOSED);
    CHECK(h2_pal_queue_recv(api, queue, &out, 0u) == H2_PAL_ERR_CLOSED);
    h2_pal_queue_destroy(api, queue);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void task_entry(void *ctx)
{
    int *flag = (int *)ctx;
    *flag = 42;
}

static void test_task_start_uses_sdk_task_and_join_is_unsupported(void)
{
    const h2_pal_task_api_t *api = h2_jieli_br23_platform_task_api();
    const h2_pal_task_options_t options = {.name = "pal_e2e", .min_stack_size = 8192u};
    h2_pal_task_t *task = NULL;
    int flag = 0;
    h2_jieli_fake_reset();
    CHECK(h2_pal_task_start(api, &options, task_entry, &flag, &task) == H2_PAL_OK);
    CHECK(task != NULL);
    CHECK(h2_jieli_fake_task_create_calls() == 1);
    CHECK(strcmp(h2_jieli_fake_last_task_name(), "pal_e2e") == 0);
    CHECK(h2_jieli_fake_last_task_stack_bytes() == 8192u);
    h2_jieli_fake_run_last_task_once();
    CHECK(flag == 42);
    CHECK(h2_pal_task_join(api, task) == H2_PAL_ERR_UNSUPPORTED);
    h2_jieli_fake_fail_task_create(1);
    CHECK(h2_pal_task_start(api, NULL, task_entry, &flag, &task) == H2_PAL_ERR_TASK);
}

static int timer_fires;

static void timer_callback(void *user, h2_pal_timer_t *timer)
{
    (void)timer;
    (*(int *)user)++;
}

static void test_timer_one_shot_and_periodic(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    h2_pal_timer_config_t config = {
        .name = "t",
        .period_ms = 100u,
        .flags = H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_callback,
        .cb_user = &timer_fires,
    };
    h2_pal_timer_t *timer = NULL;
    int running = 0;
    h2_jieli_fake_reset();
    timer_fires = 0;
    CHECK(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    CHECK(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK);
    CHECK(running == 1);
    h2_jieli_fake_advance_ms(100u);
    h2_jieli_fake_run_timers();
    CHECK(timer_fires == 1);
    CHECK(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK);
    CHECK(running == 0);
    h2_jieli_fake_advance_ms(100u);
    h2_jieli_fake_run_timers();
    CHECK(timer_fires == 1);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);

    config.flags = H2_PAL_TIMER_FLAG_REPEAT;
    timer_fires = 0;
    CHECK(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    CHECK(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK);
    CHECK(running == 0);
    CHECK(h2_pal_timer_start(api, timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(250u);
    h2_jieli_fake_run_timers();
    h2_jieli_fake_run_timers();
    CHECK(timer_fires == 2);
    CHECK(h2_pal_timer_set_period_ms(api, timer, 50u) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(50u);
    h2_jieli_fake_run_timers();
    CHECK(timer_fires == 3);
    CHECK(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
    CHECK(h2_jieli_fake_timer_count() == 0u);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    /* destroy() hands the storage to a reclaim timeout on the owner task. */
    CHECK(h2_jieli_fake_timer_count() == 1u);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(h2_jieli_fake_timer_count() == 0u);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static const h2_pal_timer_api_t *s_destroy_api;
static h2_pal_timer_t *s_destroy_timer;
static int s_destroy_fires;

static void timer_destroying_callback(void *user, h2_pal_timer_t *timer)
{
    (void)user;
    s_destroy_fires++;
    /* Destroying from inside the callback must defer the free until the
     * callback returns instead of freeing the timer under our feet. */
    CHECK(h2_pal_timer_destroy(s_destroy_api, timer) == H2_PAL_OK);
}

static void test_timer_destroy_from_callback_defers_release(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    const h2_pal_timer_config_t config = {
        .name = "d",
        .period_ms = 10u,
        .flags = H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_destroying_callback,
        .cb_user = NULL,
    };
    h2_jieli_fake_reset();
    s_destroy_api = api;
    s_destroy_fires = 0;
    CHECK(h2_pal_timer_create(api, &config, &s_destroy_timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(10u);
    h2_jieli_fake_run_timers();
    CHECK(s_destroy_fires == 1);
    /* The periodic timer is gone; only the reclaim timeout remains and the
     * storage stays valid until it runs. */
    CHECK(h2_jieli_fake_timer_count() == 1u);
    CHECK(h2_jieli_fake_live_allocations() == 1);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(h2_jieli_fake_timer_count() == 0u);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static int s_raced_fires;

static void timer_raced_callback(void *user, h2_pal_timer_t *timer)
{
    (void)user;
    (void)timer;
    s_raced_fires++;
}

static void destroy_dispatched_timer(void)
{
    /* Runs after the SDK dispatched the fire but before timer_fire entered,
     * i.e. destroy() from the owner task racing the queued callback. */
    CHECK(h2_pal_timer_destroy(s_destroy_api, s_destroy_timer) == H2_PAL_OK);
}

static void test_timer_destroy_racing_dispatched_callback(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    const h2_pal_timer_config_t config = {
        .name = "r",
        .period_ms = 10u,
        .flags = H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_raced_callback,
        .cb_user = NULL,
    };
    h2_jieli_fake_reset();
    s_destroy_api = api;
    s_raced_fires = 0;
    CHECK(h2_pal_timer_create(api, &config, &s_destroy_timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(10u);
    h2_jieli_fake_set_timer_dispatch_hook(destroy_dispatched_timer);
    h2_jieli_fake_run_timers();
    /* The dispatched fire saw the destroyed flag on still-valid storage and
     * did not invoke the user callback; the reclaim timeout is pending. */
    CHECK(s_raced_fires == 0);
    CHECK(h2_jieli_fake_timer_count() == 1u);
    CHECK(h2_jieli_fake_live_allocations() == 1);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(s_raced_fires == 0);
    CHECK(h2_jieli_fake_timer_count() == 0u);
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

/* Distinct task handles; the SDK may give two tasks the same name, so the
 * provider keys ownership on the handle. */
static int s_task_owner;
static int s_task_other;

static void test_timer_destroy_from_other_task_is_rejected(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    const h2_pal_timer_config_t config = {
        .name = "x",
        .period_ms = 10u,
        .flags = H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_raced_callback,
        .cb_user = NULL,
    };
    h2_pal_timer_t *timer = NULL;
    h2_jieli_fake_reset();
    s_raced_fires = 0;
    h2_jieli_fake_set_current_task(&s_task_owner);
    CHECK(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    /* A fire is already queued to the owner task at this point; a destroy
     * from another task cannot order its reclaim behind it. */
    h2_jieli_fake_set_current_task(&s_task_other);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_jieli_fake_live_allocations() == 1);
    h2_jieli_fake_advance_ms(10u);
    h2_jieli_fake_run_timers();
    /* The timer survived the rejected destroy and still fires normally. */
    CHECK(s_raced_fires == 1);
    h2_jieli_fake_set_current_task(&s_task_owner);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void test_timer_start_from_other_task_is_rejected(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    const h2_pal_timer_config_t config = {
        .name = "m",
        .period_ms = 10u,
        .flags = H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_raced_callback,
        .cb_user = NULL,
    };
    h2_pal_timer_t *timer = NULL;
    h2_jieli_fake_reset();
    s_raced_fires = 0;
    h2_jieli_fake_set_current_task(&s_task_owner);
    CHECK(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    /* Another task stops the timer — a fire may still be queued to the
     * original owner — and tries to take it over. Ownership must not move,
     * otherwise that task could order the reclaim behind its own callbacks. */
    h2_jieli_fake_set_current_task(&s_task_other);
    /* Even while the timer is still running a foreign start() is rejected
     * rather than answered with the already-running OK. */
    CHECK(h2_pal_timer_start(api, timer) == H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
    CHECK(h2_pal_timer_start(api, timer) == H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_timer_reset(api, timer) == H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_jieli_fake_live_allocations() == 1);
    /* The owner can re-arm and release it. */
    h2_jieli_fake_set_current_task(&s_task_owner);
    CHECK(h2_pal_timer_start(api, timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(10u);
    h2_jieli_fake_run_timers();
    CHECK(s_raced_fires == 1);
    CHECK(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void test_timer_destroy_without_timeout_slot_keeps_timer(void)
{
    const h2_pal_timer_api_t *api = h2_jieli_br23_platform_timer_api();
    const h2_pal_timer_config_t config = {
        .name = "s",
        .period_ms = 10u,
        .flags = H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = timer_raced_callback,
        .cb_user = NULL,
    };
    h2_pal_timer_t *victim = NULL;
    h2_pal_timer_t *others[H2_JIELI_FAKE_TIMER_CAPACITY];
    size_t i;
    h2_jieli_fake_reset();
    s_raced_fires = 0;
    h2_jieli_fake_set_current_task(&s_task_owner);
    /* Started once (so it has an owner task and may have a queued fire) and
     * then stopped, so destroy() needs a fresh SDK slot for the reclaim. */
    CHECK(h2_pal_timer_create(api, &config, &victim) == H2_PAL_OK);
    CHECK(h2_pal_timer_stop(api, victim) == H2_PAL_OK);
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        others[i] = NULL;
        CHECK(h2_pal_timer_create(api, &config, &others[i]) == H2_PAL_OK);
    }
    CHECK(h2_jieli_fake_timer_count() == H2_JIELI_FAKE_TIMER_CAPACITY);
    /* No slot left for the reclaim: the timer must stay alive and owned by
     * the caller instead of being freed under a possibly queued callback. */
    CHECK(h2_pal_timer_destroy(api, victim) == H2_PAL_ERR_UNAVAILABLE);
    CHECK(h2_jieli_fake_live_allocations() == (int)H2_JIELI_FAKE_TIMER_CAPACITY + 1);
    /* Freeing one slot lets the retry from the owner task succeed. */
    CHECK(h2_pal_timer_stop(api, others[0]) == H2_PAL_OK);
    CHECK(h2_pal_timer_destroy(api, victim) == H2_PAL_OK);
    h2_jieli_fake_advance_ms(1u);
    h2_jieli_fake_run_timers();
    CHECK(h2_jieli_fake_live_allocations() == (int)H2_JIELI_FAKE_TIMER_CAPACITY);
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        CHECK(h2_pal_timer_destroy(api, others[i]) == H2_PAL_OK);
        h2_jieli_fake_advance_ms(1u);
        h2_jieli_fake_run_timers();
    }
    CHECK(h2_jieli_fake_live_allocations() == 0);
}

static void test_firmware_info_reports_build_version(void)
{
    const h2_pal_firmware_info_api_t *api = h2_jieli_br23_platform_firmware_info_api();
    h2_pal_firmware_info_t info;
    CHECK(h2_pal_firmware_info_get_current(api, &info) == H2_PAL_OK);
    CHECK(info.version[0] != '\0');
}

int main(void)
{
    test_mem_round_trip();
    test_log_formats_level_scope_and_crlf();
    test_log_truncates_long_messages_without_overflow();
    test_log_bounds_long_scope_and_message_together();
    test_time_extends_32bit_wrap_and_sleeps();
    test_sync_mutex_and_semaphore();
    test_queue_fifo_full_timeout_latest_and_close();
    test_task_start_uses_sdk_task_and_join_is_unsupported();
    test_timer_one_shot_and_periodic();
    test_timer_destroy_from_callback_defers_release();
    test_timer_destroy_racing_dispatched_callback();
    test_timer_destroy_from_other_task_is_rejected();
    test_timer_start_from_other_task_is_rejected();
    test_timer_destroy_without_timeout_slot_keeps_timer();
    test_firmware_info_reports_build_version();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
