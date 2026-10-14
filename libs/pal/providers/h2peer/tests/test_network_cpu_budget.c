#include "h2_peer_internal.h"
#include "h2_peer_task_names.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Capture a command through the public API, then sustain that same work on
 * the production worker. A deterministic clock models CPU time; no socket,
 * media output or performance result is invented by this scheduler test. */
static const h2_pal_queue_api_t *real_queue;
static h2_pal_queue_t *commands, *responses;
static size_t command_size, response_size;
static void *captured_command;
static h2_pal_task_entry_t worker;
static void *worker_context;
static h2_pal_webrtc_peer_t *peer;
static bool flood, fail_clock, backward_clock, fail_sleep;
static bool pending_response_exit, late_response;
static unsigned rounds, responses_sent, sleeps, starts, joins;
static uint64_t now_us;
static int task_marker;

static int create_queue(void *user, const h2_pal_queue_config_t *config,
                        h2_pal_queue_t **out) {
    int rc = real_queue->vtable->create(user, config, out);
    if (rc == H2_PAL_OK && strcmp(config->name, "h2peer/net/commands") == 0) {
        commands = *out;
        command_size = config->item_size;
    }
    if (rc == H2_PAL_OK && strcmp(config->name, "h2peer/net/responses") == 0) {
        responses = *out;
        response_size = config->item_size;
    }
    return rc;
}
static int send_queue(void *user, h2_pal_queue_t *queue, const void *item,
                      uint32_t timeout_ms) {
    if (queue == commands && !flood) {
        assert(captured_command == NULL);
        captured_command = malloc(command_size);
        assert(captured_command != NULL);
        memcpy(captured_command, item, command_size);
        return pending_response_exit
                   ? real_queue->vtable->send(user, queue, item, timeout_ms)
                   : H2_PAL_OK;
    }
    if (queue == responses && flood) {
        ++responses_sent;
        return H2_PAL_OK;
    }
    return real_queue->vtable->send(user, queue, item, timeout_ms);
}
static int receive_queue(void *user, h2_pal_queue_t *queue, void *item,
                         uint32_t timeout_ms) {
    if (queue == responses && pending_response_exit) {
        /* A synchronous command is already queued when the production worker
         * fails its scheduling API before dequeuing it. */
        assert(timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER);
        worker(worker_context);
        pending_response_exit = false;
        if (late_response) {
            void *reply = calloc(1u, response_size);
            assert(reply != NULL);
            assert(real_queue->vtable->send(user, queue, reply,
                                            H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
            free(reply);
        }
        return H2_PAL_ERR_TIMEOUT;
    }
    if (queue == responses && !flood &&
        h2_atomic_load(&peer->network_stopped)) {
        assert(timeout_ms == H2_PAL_QUEUE_NO_WAIT);
        return real_queue->vtable->recv(user, queue, item, timeout_ms);
    }
    if (queue == responses && !flood) {
        memset(item, 0, response_size);
        return H2_PAL_OK;
    }
    if (queue == commands && flood) {
        assert(timeout_ms == H2_PAL_QUEUE_NO_WAIT);
        memcpy(item, captured_command, command_size);
        now_us += 2000u;
        if (backward_clock && rounds == 19u) now_us = 0u;
        if (++rounds == 40u) h2_atomic_store(&peer->network_stop, 1);
        return H2_PAL_OK;
    }
    return real_queue->vtable->recv(user, queue, item, timeout_ms);
}
static int start_task(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context, h2_pal_task_t **out) {
    (void)user;
    assert(strcmp(options->name, h2_peer_network_task_name) == 0);
    worker = entry;
    worker_context = context;
    *out = (h2_pal_task_t *)(void *)&task_marker;
    ++starts;
    return H2_PAL_OK;
}
static int join_task(void *user, h2_pal_task_t *task) {
    (void)user;
    assert(task == (h2_pal_task_t *)(void *)&task_marker);
    ++joins;
    return H2_PAL_OK;
}
static h2_pal_result_t monotonic(void *user, uint64_t *out) {
    (void)user;
    if (fail_clock) return H2_PAL_ERR_IO;
    *out = now_us;
    return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
    (void)user;
    assert(ms > 0u && ms <= 1u);
    now_us += (uint64_t)ms * 1000u;
    ++sleeps;
    return fail_sleep ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
}
static void run_case(bool clock_failure, bool clock_backwards, bool sleep_failure,
                     bool pending_call, bool final_reply) {
    commands = responses = NULL;
    captured_command = NULL;
    worker = NULL;
    peer = NULL;
    flood = false;
    pending_response_exit = pending_call;
    late_response = final_reply;
    fail_clock = clock_failure;
    fail_sleep = sleep_failure;
    backward_clock = clock_backwards;
    rounds = responses_sent = sleeps = starts = joins = 0u;
    now_us = 0u;
    real_queue = h2_desktop_platform_queue_api();
    h2_pal_queue_vtable_t qv = *real_queue->vtable;
    qv.create = create_queue;
    qv.send = send_queue;
    qv.recv = receive_queue;
    h2_pal_queue_api_t queue = {real_queue->user, &qv};
    const h2_pal_task_api_t *real_task = h2_desktop_platform_task_api();
    h2_pal_task_vtable_t tv = *real_task->vtable;
    tv.start = start_task;
    tv.join = join_task;
    h2_pal_task_api_t task = {real_task->user, &tv};
    const h2_pal_time_api_t *real_time = h2_desktop_platform_time_api();
    h2_pal_time_vtable_t time_v = *real_time->vtable;
    time_v.get_monotonic_us = monotonic;
    time_v.sleep_ms = sleep_ms;
    h2_pal_time_api_t time = {real_time->user, &time_v};
    h2_peer_config_t config = {
        .mem = h2_desktop_platform_default_allocator(),
        .log = h2_desktop_platform_log_api(),
        .net = h2_pal_unsupported_net_api(),
        .queue = &queue, .sync = h2_desktop_platform_sync_api(),
        .task = &task, .time = &time,
        .crypto = h2_pal_unsupported_crypto_api(),
        .dtls = h2_pal_unsupported_dtls_api(),
        .sctp = h2_pal_unsupported_sctp_api(),
    };
    h2_peer_t *owner = NULL;
    assert(h2_peer_create(&config, &owner) == H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
    h2_pal_webrtc_track_t track = {0};
    int call_rc = h2_pal_webrtc_peer_unset_track(api, peer, &track);
    assert(call_rc ==
           (pending_call && !final_reply ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
    assert(captured_command != NULL && worker != NULL);
    if (!pending_call) {
        flood = true;
        worker(worker_context);
    }
    assert(responses_sent == rounds);
    if (sleep_failure) {
        assert((pending_call ? rounds == 0u : rounds > 0u && rounds < 40u) &&
               sleeps == 1u);
        h2_pal_webrtc_event_t event = {0};
        assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
        assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR &&
               event.error == H2_PAL_ERR_UNSUPPORTED);
        h2_pal_webrtc_event_release(&event);
    } else {
        assert(rounds == 40u);
        if (clock_failure) assert(sleeps == rounds);
        else assert(sleeps >= 7u && sleeps <= 10u);
    }
    flood = false;
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && starts == 1u && joins == 1u);
    free(captured_command);
}
int main(void) {
    run_case(false, false, false, false, false);
    run_case(true, false, false, false, false);
    run_case(false, true, false, false, false);
    run_case(false, false, true, false, false);
    run_case(true, false, true, true, false);
    run_case(true, false, true, true, true);
    return 0;
}
