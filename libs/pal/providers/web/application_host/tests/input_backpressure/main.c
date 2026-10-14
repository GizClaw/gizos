#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include <assert.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int mode, downs, ups, position;
static _Atomic int pause_edges, blocked, retain_requested;
static _Atomic int fault_requested;
static _Atomic unsigned pace_faults;
static _Atomic int health_gate, health_active, health_seen;
static _Atomic unsigned accepted, timeouts, busy;
static _Atomic int consumer_paused;
static _Atomic unsigned paused_accepts, saturation_rejections;
static const h2_pal_queue_api_t *base_queue;
static h2_pal_queue_api_t queue_api;
static h2_pal_queue_vtable_t queue_vtable;
static h2_pal_queue_t *push_queue;
static const h2_pal_time_api_t *base_time;
static h2_pal_time_api_t time_api;
static h2_pal_time_vtable_t time_vtable;
static _Thread_local int input_worker;

EMSCRIPTEN_KEEPALIVE void input_test_pause(int pause) {
    if (pause) {
        atomic_store(&consumer_paused, 0);
        atomic_store(&paused_accepts, 0u);
        atomic_store(&saturation_rejections, 0u);
    }
    atomic_store(&pause_edges, pause);
}
EMSCRIPTEN_KEEPALIVE int input_test_paused(void) {
    return atomic_load(&consumer_paused);
}
EMSCRIPTEN_KEEPALIVE int input_test_saturated(void) {
    return atomic_load(&paused_accepts) == 16u &&
           atomic_load(&saturation_rejections) > 0u;
}
EMSCRIPTEN_KEEPALIVE void input_test_block(int kind) {
    atomic_store(&blocked, kind == 1 ? H2_PAL_ERR_TIMEOUT
                          : kind == 2 ? H2_PAL_ERR_BUSY
                          : kind == 3 ? H2_PAL_ERR_IO : H2_PAL_OK);
}
EMSCRIPTEN_KEEPALIVE void input_test_retain(void) {
    atomic_store(&retain_requested, 1);
}
EMSCRIPTEN_KEEPALIVE void input_test_fault(void) {
    atomic_store(&fault_requested, 1);
}
EMSCRIPTEN_KEEPALIVE int input_test_health(int action) {
    if (action != 0) atomic_store(&health_gate, action == 1);
    return atomic_load(&health_active);
}

static int queue_create(void *user, const h2_pal_queue_config_t *config,
                         h2_pal_queue_t **out) {
    int rc = base_queue->vtable->create(user, config, out);
    if (rc == H2_PAL_OK && strcmp(config->name, "h2-runtime-button-push-edges") == 0) {
        assert(config->item_count == 16u);
        push_queue = *out;
        puts("INPUT raw_capacity=16");
    }
    return rc;
}
static int queue_recv(void *user, h2_pal_queue_t *queue, void *item,
                       uint32_t timeout) {
    if (queue == push_queue) input_worker = 1;
    if (queue == push_queue && atomic_load(&pause_edges)) {
        atomic_store(&consumer_paused, 1);
        return H2_PAL_ERR_TIMEOUT;
    }
    return base_queue->vtable->recv(user, queue, item, timeout);
}
static int queue_send(void *user, h2_pal_queue_t *queue, const void *item,
                       uint32_t timeout) {
    const int rc = base_queue->vtable->send(user, queue, item, timeout);
    if (queue == push_queue && atomic_load(&pause_edges)) {
        if (rc == H2_PAL_OK) atomic_fetch_add(&paused_accepts, 1u);
        if (rc == H2_PAL_ERR_TIMEOUT && atomic_load(&paused_accepts) == 16u)
            atomic_fetch_add(&saturation_rejections, 1u);
    }
    return rc;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
    if (input_worker && atomic_exchange(&fault_requested, 0)) {
        atomic_fetch_add(&pace_faults, 1u);
        return H2_PAL_ERR_IO;
    }
    return base_time->vtable->sleep_ms(user, ms);
}
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
    (void)user;
    base_queue = config->queue;
    queue_vtable = *base_queue->vtable;
    queue_vtable.create = queue_create;
    queue_vtable.recv = queue_recv;
    queue_vtable.send = queue_send;
    queue_api = (h2_pal_queue_api_t){base_queue->user, &queue_vtable};
    config->queue = &queue_api;
    base_time = config->time;
    time_vtable = *base_time->vtable;
    time_vtable.sleep_ms = sleep_ms;
    time_api = (h2_pal_time_api_t){base_time->user, &time_vtable};
    config->time = &time_api;
    // Keep derived event delivery out of this raw-edge admission experiment.
    config->event_queue_capacity = 256u;
    return H2_PAL_OK;
}
static h2_pal_result_t release_runtime(void *user) {
    (void)user;
    assert(atomic_load(&health_active) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t input_status(h2_runtime_t *runtime,
                                    h2_runtime_input_status_t *out) {
    if (mode == 9 && atomic_load(&health_gate)) {
        assert(!emscripten_is_main_runtime_thread());
        atomic_store(&health_active, 1);
        atomic_store(&health_seen, 1);
        while (atomic_load(&health_gate))
            assert(h2_pal_time_sleep_ms(base_time, 1u) == H2_PAL_OK);
        h2_pal_result_t rc = h2_runtime_input_status(runtime, out);
        atomic_store(&health_active, 0);
        return rc;
    }
    return h2_runtime_input_status(runtime, out);
}
static h2_pal_result_t push_edge(h2_runtime_t *runtime,
                                 h2_pal_periph_id_t periph,
                                 h2_runtime_button_edge_t edge) {
    int rc = edge == H2_RUNTIME_BUTTON_EDGE_UP ? atomic_load(&blocked) : 0;
    if (rc == H2_PAL_OK)
        rc = h2_runtime_button_push_edge(runtime, periph, edge);
    if (rc == H2_PAL_OK) atomic_fetch_add(&accepted, 1u);
    if (rc == H2_PAL_ERR_TIMEOUT) atomic_fetch_add(&timeouts, 1u);
    if (rc == H2_PAL_ERR_BUSY) atomic_fetch_add(&busy, 1u);
    return rc;
}
#define h2_runtime_button_push_edge push_edge
#define h2_runtime_input_status input_status
#include "libs/pal/providers/web/application_host/src/h2_web_application_host.c"
#undef h2_runtime_button_push_edge
#undef h2_runtime_input_status

/* Observe the private admission boundary to force a read/teardown overlap;
 * the release hook above asserts the externally meaningful owner order. */
EMSCRIPTEN_KEEPALIVE int input_test_deactivated(void) {
    h2_web_application_host_t *host = atomic_load(&s_host);
    return host != NULL && atomic_load(&host->runtime) == NULL;
}

/* clang-format off */
EM_JS(void, read_mode,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () =>
      Number(new URLSearchParams(location.search).get('mode') || 0));
});
EM_JS(void, bind_failure,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], null, () => {
    const bind = globalThis.h2WebApplicationHostBindButton;
    const native = Module._h2_web_application_host_button;
    let calls = 0;
    Module._h2_web_application_host_button = (...args) => { calls++; return native(...args); };
    const original = console.log;
    console.log = (...args) => {
      if (args.map(String).join(' ').includes('INPUT_TEST mode=7 PASS')) {
        const before = calls;
        setTimeout(() => console.log(calls === before ? 'INPUT_LATE_CALLBACK PASS' :
            'INPUT_FLOW FAIL callback after rollback'), 30);
      }
      original.apply(console, args);
    };
    globalThis.h2WebApplicationHostBindButton = (...args) => {
      if (args[0] === 1) throw new Error('controlled second bind failure');
      bind(...args);
      Module._input_test_pause(1);
      for (let i = 0; i < 20; i++) {
        for (const type of ['keydown', 'keyup'])
          globalThis.dispatchEvent(new KeyboardEvent(type, {key: ']', bubbles: true}));
      }
      setTimeout(() => Module._input_test_pause(0), 80);
    };
  });
});
/* clang-format on */

static h2_pal_result_t entry(h2_application_host_t *host,
                             h2_runtime_t *runtime, void *user) {
    (void)user;
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {.payload = payload, .payload_capacity = sizeof(payload)};
    uint16_t pixels[64 * 64];
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0xffe0};
    int painted = -1, faulted = 0;
    assert(h2_pal_display_open(runtime->display) == H2_PAL_OK);
    assert(h2_application_host_ready(host) == H2_PAL_OK);
    puts("INPUT_READY");
    while (!h2_application_host_should_stop(host)) {
        if (atomic_load(&retain_requested)) {
            assert(h2_application_host_retain(host, NULL, H2_PAL_ERR_TASK) == H2_PAL_OK);
            return H2_PAL_OK;
        }
        for (unsigned i = 0; i < 256u; ++i) {
            int rc = h2_runtime_poll_event(runtime, &event);
            if (mode == 8 && rc == H2_PAL_ERR_CLOSED) { faulted = 1; break; }
            if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) break;
            assert(rc == H2_PAL_OK);
            if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN &&
                event.timestamp_ms == ((h2_runtime_button_down_event_t *)payload)->pressed_at_ms) {
                ++downs;
                position = (position + (event.component_id == 1 ? 1 : 4)) % 5;
            } else if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP) {
                ++ups;
            } else continue;
            printf("INPUT_STATE down=%d up=%d position=%d component=%u edge=%s\n",
                downs, ups, position, (unsigned)event.component_id,
                event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP ? "U" : "D");
        }
        if (faulted) break;
        if (painted != position) {
            for (size_t i = 0; i < 64u * 64u; ++i) pixels[i] = colors[position];
            const h2_display_rect_t rect = {0, 0, 64, 64};
            assert(h2_pal_display_draw_bitmap(runtime->display, &rect, pixels,
                       64u * sizeof(*pixels), H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
            assert(h2_pal_display_present(runtime->display) == H2_PAL_OK);
            painted = position;
        }
        assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
    }
    assert(h2_pal_display_close(runtime->display) == H2_PAL_OK);
    return faulted ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int main(void) {
    mode = h2_web_main_call(read_mode, NULL).i32;
    if (mode == 7) (void)h2_web_main_call(bind_failure, NULL);
    static const h2_web_application_host_button_t buttons[] = {
        {.component_id = 1, .key = "]", .name = "forward"},
        {.component_id = 2, .key = "[", .name = "reverse"}};
    static const h2_web_application_host_hardware_t hardware = {
        .configure_runtime = configure, .release_runtime = release_runtime};
    const h2_web_application_host_config_t config = {
        .name = "input-backpressure", .buttons = buttons, .button_count = 2,
        .hardware = &hardware};
    int rc = h2_web_application_host_run(&config, entry, NULL);
    assert(rc == (mode == 3 || mode == 4 || mode == 7 || mode == 8 ? 1 : mode == 5 ? 2 : 0));
    if (mode == 0) {
        assert(atomic_load(&timeouts) > 0u);
        assert(atomic_load(&paused_accepts) == 16u);
        assert(atomic_load(&saturation_rejections) > 0u);
        assert(atomic_load(&accepted) == 146u);
        assert(downs == 73 && ups == 73 && position == 1);
    } else if (mode == 1) {
        assert(atomic_load(&timeouts) > 0u && atomic_load(&busy) > 0u);
        assert(atomic_load(&accepted) == 6u && downs == 3 && ups == 3);
    } else if (mode == 2) {
        assert(atomic_load(&timeouts) > 0u && atomic_load(&accepted) == 2u);
    } else if (mode == 6) {
        assert(atomic_load(&timeouts) > 0u && atomic_load(&accepted) == 4u);
        assert(downs == 2 && ups == 2);
    } else if (mode == 7) {
        assert(atomic_load(&timeouts) > 0u && atomic_load(&accepted) == 40u);
        assert(downs == 0 && ups == 0);
    } else if (mode == 8) {
        assert(atomic_load(&timeouts) > 0u && atomic_load(&accepted) == 1u);
        assert(atomic_load(&pace_faults) == 1u);
    } else if (mode == 9) {
        assert(atomic_load(&health_seen) == 1 && atomic_load(&health_active) == 0);
        assert(atomic_load(&accepted) == 2u);
    }
    printf("INPUT_TEST mode=%d PASS accepted=%u timeout=%u busy=%u\n", mode,
        atomic_load(&accepted), atomic_load(&timeouts), atomic_load(&busy));
    return 0;
}
