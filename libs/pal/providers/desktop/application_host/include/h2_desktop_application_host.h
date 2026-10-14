#ifndef H2_DESKTOP_APPLICATION_HOST_H
#define H2_DESKTOP_APPLICATION_HOST_H

#include "h2/pal/os/h2_pal_json.h"
#include "h2_application_host.h"
#include "h2_corehttp.h"
#include "h2_desktop_app_support_c.h"
#include "h2_desktop_platform.h"
#include "h2_portaudio.h"
#include "h2_sdl3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_application_host h2_desktop_application_host_t;

enum {
  H2_DESKTOP_HOST_CRYPTO = 1u << 0,
  H2_DESKTOP_HOST_HTTP = 1u << 1,
  H2_DESKTOP_HOST_MQTT = 1u << 2,
  H2_DESKTOP_HOST_WEBRTC = 1u << 3,
  H2_DESKTOP_HOST_JSON = 1u << 4,
  H2_DESKTOP_HOST_AUDIO = 1u << 5,
  H2_DESKTOP_HOST_PREFERENCE = 1u << 6,
  H2_DESKTOP_HOST_LVGL = 1u << 7,
  H2_DESKTOP_HOST_BLEIKCP = 1u << 8,
};

/** Host-owned platform resources. Zero window dimensions select headless
 * operation. A zero Runtime descriptor selects the canonical Desktop PALs;
 * an injected descriptor remains useful for provider-specific fault probes.
 * Hooks, mount strings and opaque contexts are borrowed through completion,
 * indefinitely on retention. Runtime cleanup precedes resource release.
 */
typedef struct h2_desktop_application_host_config {
  h2_sdl3_config_t window;
  h2_runtime_config_t runtime;
  void *user;
  /** Bounded main-thread callback; event is borrowed only during this call.
   * Deactivate closes admission and drains callbacks before Runtime cleanup. */
  h2_pal_result_t (*event)(void *user, const h2_sdl3_event_t *event);
  unsigned services;
  const h2_desktop_filesystem_mount_t *mounts;
  size_t mount_count;
  const char *preference_path;
  const h2_desktop_peripheral_config_t *peripherals;
  size_t peripheral_count;
  const h2_runtime_component_mapping_entry_t *mappings;
  size_t mapping_count;
  /** Optional service allocator, independent of Host/Runtime allocation. */
  const h2_pal_mem_api_t *service_allocator;
  size_t mqtt_records;
  h2_corehttp_config_t http;
  int require_real_audio;
  h2_portaudio_capture_hooks_t audio_capture;
  h2_sdl3_render_capture_fn render_capture;
  h2_sdl3_frame_capture_fn frame_capture;
  /** Start Runtime input before App entry when a component mapper is set. */
  int input;
  /** GUI Apps using SDL/LVGL directly run on the process main thread.
   * Their frame loop must call the Desktop should_stop helper below. */
  int entry_on_main;
  /** Optional shared native status surface, shown until close on App error.
   * status_on_error keeps successful App rendering untouched. */
  const char *status_name;
  int status_on_error;
} h2_desktop_application_host_config_t;

/** Complete create/run/checked cleanup. Call on the process main thread.
 * App work runs on the shared Host task; SDL pumping stays on this thread.
 * RETAINED is reported in result and keeps every still-borrowed owner alive.
 * app.provider must be NULL; allocator defaults to Desktop memory.
 * H2_DESKTOP_RUN_MS supplies a bounded cooperative duration when app.run_ms
 * is zero. GUI Apps observe it at their should_stop/wait frame boundaries.
 */
h2_pal_result_t h2_desktop_application_host_run(
    const h2_desktop_application_host_config_t *config,
    const h2_application_host_config_t *app,
    h2_application_host_result_t *result);
const h2_pal_json_api_t *
h2_desktop_application_host_json(h2_application_host_t *host);
/** Borrowed LVGL display/keyboard/wheel handles; NULL without LVGL. */
void *h2_desktop_application_host_lvgl_display(h2_application_host_t *host);
void *h2_desktop_application_host_lvgl_keyboard(h2_application_host_t *host);
void *h2_desktop_application_host_lvgl_wheel(h2_application_host_t *host);
/** Main-thread GUI frame boundary: pump SDL/input and observe shared stop.
 * Headless/worker Apps use h2_application_host_should_stop instead. */
int h2_desktop_application_host_should_stop(void *host);
/** Keep a completed static scene visible until close/cooperative stop. */
h2_pal_result_t h2_desktop_application_host_wait(h2_application_host_t *host);
/** Read the cached SDL pointer. The owner pump updates it; thread-safe. */
h2_pal_result_t
h2_desktop_application_host_pointer(h2_application_host_t *host,
                                    h2_sdl3_pointer_state_t *state);
/** Borrow native providers for scoped diagnostics/recording registrations.
 * Their handles stay owned by Host; unregister observers before entry exits. */
h2_sdl3_t *h2_desktop_application_host_sdl(h2_application_host_t *host);
h2_portaudio_t *
h2_desktop_application_host_portaudio(h2_application_host_t *host);

/** Create on the process main thread, then inject api() into shared Host.
 * create/destroy and Host run must execute on that same main thread.
 */
h2_pal_result_t h2_desktop_application_host_create(
    const h2_desktop_application_host_config_t *config,
    h2_desktop_application_host_t **out_provider);
const h2_pal_application_host_api_t *
h2_desktop_application_host_api(h2_desktop_application_host_t *provider);
/** BUSY retains a provider whose resource graph has not been released. */
h2_pal_result_t
h2_desktop_application_host_destroy(h2_desktop_application_host_t **provider);

#ifdef __cplusplus
}
#endif
#endif
