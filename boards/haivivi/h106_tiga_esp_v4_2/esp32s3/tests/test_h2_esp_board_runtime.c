/* Host coverage for board Runtime teardown and task-policy configuration.
 * Links the production translation unit against SDK/provider stubs and checks
 * that the arena is initialized before the ESP task provider is configured. */

#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_private.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_platform_image.h"

#include "h2_corehttp.h"
#include "esp_mac.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

_Static_assert(H2_TIGA_ESP_ARENA_BYTES == 4u * 1024u * 1024u,
               "default arena cap is 4 MiB, not a fixed reservation");
_Static_assert(H2_TIGA_ESP_ARENA_SYSTEM_RESERVE_BYTES == (896u - 128u) * 1024u,
               "system reserve excludes removed catalog residents");
_Static_assert(H2_TIGA_ESP_ARENA_SMALL_REQUEST_MAX == 32u * 1024u,
               "default small request limit is 32 KiB");
_Static_assert(H2_TIGA_ESP_ARENA_SMALL_POOL_BYTES == 1024u * 1024u,
               "default small pool reservation is 1 MiB");

static void test_arena_sizing(void) {
  const size_t kib = 1024u;
  const size_t mib = 1024u * kib;
  const size_t reserve = H2_TIGA_ESP_ARENA_SYSTEM_RESERVE_BYTES;
  /* System reserve wins on an unfragmented 4.5 MiB heap. */
  assert(h2_esp_board_arena_reserved_bytes(4u * mib + 512u * kib, 4u * mib) ==
         3u * mib + 768u * kib);
  /* Cap, fragmentation, and both sources of 64 KiB round-down. */
  assert(h2_esp_board_arena_reserved_bytes(8u * mib, 8u * mib) == 4u * mib);
  assert(h2_esp_board_arena_reserved_bytes(6u * mib, 2u * mib + 123u) ==
         2u * mib);
  assert(h2_esp_board_arena_reserved_bytes(4u * mib + 123u, 4u * mib) ==
         3u * mib + 256u * kib);
  assert(h2_esp_board_arena_reserved_bytes(8u * mib, 0u) == 0u);
  assert(h2_esp_board_arena_reserved_bytes(reserve + 64u * kib - 1u, reserve) ==
         0u);
  assert(h2_esp_board_arena_reserved_bytes(reserve, reserve) == 0u);
  assert(h2_esp_board_arena_reserved_bytes(reserve - 1u, reserve - 1u) == 0u);
  assert(h2_esp_board_arena_reserved_bytes(0u, 0u) == 0u);
}

/* ---- injected state ---------------------------------------------------- */

static h2_pal_result_t s_audio_deinit_result;
static int s_audio_deinit_calls;
static int s_corehttp_destroy_calls;
static int s_fs_deinit_result;
static int s_fs_deinit_calls;
static int s_fs_clear_calls;
static int s_provider_clear_calls;
static int s_provider_clear_result;
static void* s_fs_user;
static const char* s_clear_path;
static unsigned s_fs_mount_calls;
static h2_pal_result_t s_fs_mount_result;
static const char* s_fs_mount_path;
static unsigned s_provider_prepare_calls;
static h2_pal_result_t s_provider_prepare_result;
static const char* s_provider_prepare_path;
static bool s_provider_has_prepare;
static unsigned s_arena_init_calls;
static esp_err_t s_mac_result;
static bool s_mac_zero;

static void reset_stubs(void) {
  s_audio_deinit_result = H2_PAL_OK;
  s_audio_deinit_calls = 0;
  s_corehttp_destroy_calls = 0;
  s_fs_deinit_result = H2_PAL_OK;
  s_fs_deinit_calls = 0;
  s_fs_clear_calls = 0;
  s_provider_clear_calls = 0;
  s_provider_clear_result = H2_PAL_FS_OK;
  s_fs_user = NULL;
  s_clear_path = NULL;
  s_fs_mount_calls = 0u;
  s_fs_mount_result = H2_PAL_OK;
  s_fs_mount_path = NULL;
  s_provider_prepare_calls = 0u;
  s_provider_prepare_result = H2_PAL_OK;
  s_provider_prepare_path = NULL;
  s_provider_has_prepare = false;
  s_arena_init_calls = 0;
  s_mac_result = ESP_OK;
  s_mac_zero = false;
}

/* Drives the production configure path so the module reaches the same
 * "configured" state a real boot leaves behind, without a test-only hook
 * in the production translation unit. */
static void arm_runtime(void) {
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
  assert(strcmp(config.ble_local_name, "h106-tiga-A1B2C3") == 0);
  assert(config.image == h2_esp_platform_image_api());
  assert(config.device_identity == h2_esp_platform_device_identity_api());
  assert(config.io_stream == h2_esp_platform_io_stream_api());
  assert(config.mem_external == h2_esp_board_psram_allocator());
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
  assert(type == ESP_MAC_WIFI_STA);
  static const uint8_t address[6] = {0x30, 0xed, 0xa0, 0xa1, 0xb2, 0xc3};
  memcpy(mac, address, sizeof(address));
  if (s_mac_zero) memset(mac, 0, sizeof(address));
  return s_mac_result;
}

/* ---- stubs observed by the assertions ---------------------------------- */

h2_pal_result_t h2_esp_board_audio_deinit(void) {
  s_audio_deinit_calls += 1;
  return s_audio_deinit_result;
}

int h2_esp_board_fs_deinit(void) {
  s_fs_deinit_calls += 1;
  return s_fs_deinit_result;
}

void h2_corehttp_destroy(h2_corehttp_t* http) {
  (void)http;
  s_corehttp_destroy_calls += 1;
}

static int provider_fs_clear(void* user, const char* path) {
  s_provider_clear_calls += 1;
  assert(user == s_fs_user);
  assert(path == s_clear_path);
  return s_provider_clear_result;
}

static h2_pal_result_t provider_fs_prepare(void* user, const char* path) {
  ++s_provider_prepare_calls;
  assert(user == s_fs_user);
  assert(path == s_provider_prepare_path);
  return s_provider_prepare_result;
}

static const h2_pal_fs_vtable_t s_provider_fs_vtable = {
    .clear = provider_fs_clear,
};
static const h2_pal_fs_vtable_t s_preparing_provider_fs_vtable = {
    .clear = provider_fs_clear,
    .prepare = provider_fs_prepare,
};

int h2_esp_board_fs_prepare(const char* path) {
  ++s_fs_mount_calls;
  assert(strcmp(path, s_fs_mount_path) == 0);
  return s_fs_mount_result;
}

/* ---- link-only stubs --------------------------------------------------- */

static h2_corehttp_t* const S_FAKE_HTTP =
    (h2_corehttp_t*)(void*)&s_fs_deinit_calls;

h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* config,
                                   h2_corehttp_t** out_http,
                                   h2_pal_http_api_t* out_api) {
  assert(config != NULL);
  assert(config->allocator == h2_esp_board_psram_allocator());
  (void)out_api;
  *out_http = S_FAKE_HTTP;
  return H2_PAL_OK;
}

int h2_esp_board_fs_init(h2_pal_fs_api_t* fs) {
  assert(s_arena_init_calls == 1u);
  *fs = (h2_pal_fs_api_t){
      .user = s_fs_user,
      .vtable = s_provider_has_prepare ? &s_preparing_provider_fs_vtable
                                       : &s_provider_fs_vtable,
  };
  return H2_PAL_OK;
}

int h2_esp_board_fs_clear(const char* path) {
  s_fs_clear_calls += 1;
  assert(path == s_clear_path);
  return H2_PAL_FS_ERR_IO;
}

#define STUB_PTR(type, name) \
  type name(void) { return NULL; }

STUB_PTR(const h2_pal_button_api_t*, h2_esp_board_button_api)
STUB_PTR(const h2_pal_crypto_api_t*, h2_esp_board_crypto_api)
STUB_PTR(const h2_pal_disk_api_t*, h2_esp_board_disk_api)
STUB_PTR(const h2_pal_led_api_t*, h2_esp_board_led_api)
STUB_PTR(const h2_pal_log_api_t*, h2_esp_board_log_api)
STUB_PTR(const h2_pal_nfc_api_t*, h2_esp_board_nfc_api)
STUB_PTR(const h2_pal_periph_api_t*, h2_esp_board_periph_api)
STUB_PTR(const h2_pal_power_api_t*, h2_esp_board_power_api)
STUB_PTR(const h2_pal_pref_api_t*, h2_esp_board_pref_api)
STUB_PTR(const h2_pal_queue_api_t*, h2_esp_board_queue_api)
STUB_PTR(const h2_pal_sync_api_t*, h2_esp_board_sync_api)
STUB_PTR(const h2_pal_system_event_api_t*, h2_esp_board_system_event_api)
STUB_PTR(const h2_pal_task_api_t*, h2_esp_board_task_api)
STUB_PTR(const h2_pal_time_api_t*, h2_esp_board_time_api)
STUB_PTR(const h2_pal_webrtc_api_t*, h2_esp_board_webrtc_api)
STUB_PTR(h2_pal_audio_t*, h2_esp_board_audio)
STUB_PTR(h2_pal_ble_t*, h2_esp_board_ble)
STUB_PTR(h2_pal_display_t*, h2_esp_board_display)
STUB_PTR(h2_pal_input_api_t*, h2_esp_board_input_api)
STUB_PTR(h2_pal_mem_api_t*, h2_esp_board_default_allocator)
static h2_pal_mem_api_t s_psram_allocator;
h2_pal_mem_api_t* h2_esp_board_psram_allocator(void) {
  return &s_psram_allocator;
}
STUB_PTR(h2_pal_mem_api_t*, h2_esp_board_internal_allocator)
STUB_PTR(h2_pal_modem_t*, h2_esp_board_modem)
STUB_PTR(h2_pal_pwm_switch_api_t*, h2_esp_board_pwm_switch_api)
STUB_PTR(h2_pal_wifi_ap_t*, h2_esp_board_wifi_ap)
STUB_PTR(h2_pal_wifi_settings_t*, h2_esp_board_wifi_settings)
STUB_PTR(h2_pal_wifi_sta_t*, h2_esp_board_wifi_sta)
STUB_PTR(const h2_pal_firmware_info_api_t*, h2_esp_platform_firmware_info_api)
STUB_PTR(const h2_pal_timer_api_t*, h2_esp_platform_timer_api)
STUB_PTR(const h2_pal_net_api_t*, h2_esp_platform_net_api)
STUB_PTR(const h2_pal_netif_api_t*, h2_esp_platform_netif_api)
STUB_PTR(const h2_pal_wifi_csi_api_t*, h2_esp_platform_wifi_csi)

const h2_pal_image_api_t* h2_esp_platform_image_api(void) {
  static const h2_pal_image_api_t api = {0};
  return &api;
}
const h2_pal_device_identity_api_t* h2_esp_platform_device_identity_api(void) {
  static const h2_pal_device_identity_api_t api = {0};
  return &api;
}
const h2_pal_io_stream_api_t* h2_esp_platform_io_stream_api(void) {
  static const h2_pal_io_stream_api_t api = {0};
  return &api;
}

/* ---- tests ------------------------------------------------------------- */

static void test_deinit_is_a_noop_before_configuration(void) {
  reset_stubs();
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  assert(s_audio_deinit_calls == 0);
  assert(s_corehttp_destroy_calls == 0);
  assert(s_fs_deinit_calls == 0);
}

static void test_audio_failure_still_completes_teardown(void) {
  reset_stubs();
  s_audio_deinit_result = H2_PAL_ERR_IO;
  arm_runtime();

  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO);
  /* The whole point of the fix: an audio error must not leave the HTTP
   * provider or the mounted filesystem behind. */
  assert(s_audio_deinit_calls == 1);
  assert(s_corehttp_destroy_calls == 1);
  assert(s_fs_deinit_calls == 1);

  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE);
  s_audio_deinit_result = H2_PAL_OK;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  assert(s_audio_deinit_calls == 2);
  assert(s_corehttp_destroy_calls == 1 && s_fs_deinit_calls == 1);
}

static void test_first_error_wins_over_later_failures(void) {
  reset_stubs();
  s_audio_deinit_result = H2_PAL_ERR_IO;
  s_fs_deinit_result = H2_PAL_ERR_INVALID_STATE;
  arm_runtime();

  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO);
  assert(s_fs_deinit_calls == 1);
  s_audio_deinit_result = H2_PAL_OK;
  s_fs_deinit_result = H2_PAL_OK;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  assert(s_audio_deinit_calls == 2 && s_fs_deinit_calls == 2);
}

static void test_later_failure_is_reported_when_audio_succeeds(void) {
  reset_stubs();
  s_fs_deinit_result = H2_PAL_ERR_INVALID_STATE;
  arm_runtime();

  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_INVALID_STATE);
  assert(s_corehttp_destroy_calls == 1);
  s_fs_deinit_result = H2_PAL_OK;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  assert(s_audio_deinit_calls == 1 && s_fs_deinit_calls == 2);
}

static void test_fs_clear_routes_owned_paths(void) {
  reset_stubs();
  int user = 0;
  s_fs_user = &user;
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);

  const char root[] = "/data";
  s_clear_path = root;
  assert(h2_pal_fs_clear(config.fs, root) == H2_PAL_FS_ERR_IO);
  assert(s_fs_clear_calls == 1 && s_provider_clear_calls == 0);

  const char owned[] = "/data/owned";
  s_clear_path = owned;
  s_provider_clear_result = H2_PAL_FS_ERR_NO_SPACE;
  assert(h2_pal_fs_clear(config.fs, owned) == H2_PAL_FS_ERR_NO_SPACE);
  assert(s_fs_clear_calls == 1 && s_provider_clear_calls == 1);

  /* Exercise the wrapper: the public helper rejects NULL before dispatch. */
  assert(config.fs->vtable->clear(config.fs->user, NULL) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  static const char* const rejected[] = {"/", "/dl", "/data-other", "/data/"};
  for (size_t i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
    assert(h2_pal_fs_clear(config.fs, rejected[i]) ==
           H2_PAL_FS_ERR_UNSUPPORTED);
  }
  assert(s_fs_clear_calls == 1 && s_provider_clear_calls == 1);
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
}

static void test_fs_clear_keeps_provider_after_reconfiguration(void) {
  int users[2] = {0};
  const char owned[] = "/data/owned";
  for (unsigned round = 0u; round < 2u; ++round) {
    reset_stubs();
    s_fs_user = &users[round];
    s_clear_path = owned;
    s_provider_clear_result = round == 0u ? H2_PAL_FS_OK : H2_PAL_FS_ERR_IO;
    h2_runtime_config_t config;
    assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
    assert(config.fs->vtable != &s_provider_fs_vtable);
    assert(s_provider_fs_vtable.clear == provider_fs_clear);
    assert(h2_pal_fs_clear(config.fs, owned) == s_provider_clear_result);

    /* Cached configuration must not wrap the Runtime wrapper again. */
    assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
    assert(h2_pal_fs_clear(config.fs, owned) == s_provider_clear_result);
    assert(s_fs_clear_calls == 0 && s_provider_clear_calls == 2);
    assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  }
}

static void test_fs_prepare_mounts_owned_filesystems(void) {
  reset_stubs();
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
  assert(config.fs->vtable->prepare != NULL);
  assert(s_provider_fs_vtable.prepare == NULL);
  static const char* const paths[] = {
      "/dl", "/dl/", "/dl/state.json", "/data", "/data/", "/data/app/file",
  };
  for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    s_fs_mount_path = i < 3u ? "/dl" : "/data";
    assert(h2_pal_fs_prepare(config.fs, paths[i]) == H2_PAL_OK);
  }
  assert(s_fs_mount_calls == 6u);
  s_fs_mount_result = H2_PAL_FS_ERR_IO;
  s_fs_mount_path = "/dl";
  assert(h2_pal_fs_prepare(config.fs, "/dl/stage") == H2_PAL_FS_ERR_IO);
  assert(s_fs_mount_calls == 7u);
  assert(s_fs_clear_calls == 0 && s_provider_clear_calls == 0);
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
}

static void test_fs_prepare_rejects_unowned_paths(void) {
  reset_stubs();
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
  assert(config.fs->vtable->prepare != NULL);
  assert(config.fs->vtable->prepare(config.fs->user, NULL) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  assert(config.fs->vtable->prepare(config.fs->user, "") ==
         H2_PAL_FS_ERR_INVALID_ARG);
  static const char* const paths[] = {
      "/", "dl", "data/file", "/dl-other", "/data-other", "/other/file",
  };
  for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    assert(h2_pal_fs_prepare(config.fs, paths[i]) == H2_PAL_FS_ERR_UNSUPPORTED);
  }
  assert(s_fs_mount_calls == 0u && s_provider_prepare_calls == 0u);
  assert(s_fs_clear_calls == 0 && s_provider_clear_calls == 0);
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
}

static void test_fs_prepare_preserves_provider_and_reconfiguration(void) {
  int users[3] = {0};
  for (unsigned round = 0u; round < 3u; ++round) {
    reset_stubs();
    s_fs_user = &users[round];
    s_provider_has_prepare = round != 1u;
    h2_runtime_config_t config;
    assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
    assert(config.fs->vtable->prepare != NULL);
    assert(s_preparing_provider_fs_vtable.prepare == provider_fs_prepare);
    if (s_provider_has_prepare) {
      static const char* const paths[] = {"/dl/state", "/data/file",
                                          "/other/../path"};
      for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        s_provider_prepare_path = paths[i];
        s_provider_prepare_result =
            i == 0u ? H2_PAL_OK : H2_PAL_FS_ERR_INVALID_ARG;
        assert(h2_pal_fs_prepare(config.fs, paths[i]) ==
               s_provider_prepare_result);
      }
      /* Cached configuration must retain the original callback and user. */
      assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
      assert(h2_pal_fs_prepare(config.fs, s_provider_prepare_path) ==
             s_provider_prepare_result);
      assert(s_provider_prepare_calls == 4u && s_fs_mount_calls == 0u);
    } else {
      s_fs_mount_path = "/data";
      assert(h2_pal_fs_prepare(config.fs, "/data/file") == H2_PAL_OK);
      assert(s_provider_prepare_calls == 0u && s_fs_mount_calls == 1u);
    }
    assert(s_fs_clear_calls == 0 && s_provider_clear_calls == 0);
    assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  }
}

static const h2_pal_mem_api_t s_arena_api;
static const h2_pal_mem_api_t* s_arena_mem;
static h2_esp_task_policy_config_t s_task_config;
static h2_pal_result_t s_task_configure_result;
static unsigned s_task_configure_calls;

const h2_pal_mem_api_t* h2_esp_board_arena_mem(void) {
  assert(s_arena_init_calls == 1u);
  return s_arena_mem;
}

h2_pal_result_t h2_esp_platform_task_configure(
    const h2_esp_task_policy_config_t* config) {
  assert(s_arena_init_calls == 1u);
  ++s_task_configure_calls;
  s_task_config = *config;
  return s_task_configure_result;
}

static h2_pal_result_t resolve_policy(void* user, const char* name,
                                      struct h2_esp_task_policy* policy) {
  (void)user;
  (void)name;
  (void)policy;
  return H2_PAL_ERR_NOT_FOUND;
}

static void test_task_configure(void) {
  for (unsigned mode = 0u; mode < 3u; ++mode) {
    reset_stubs();
    s_task_configure_calls = 0u;
    s_arena_mem = mode == 1u ? NULL : &s_arena_api;
    s_task_configure_result = mode == 2u ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
    h2_esp_task_policy_config_t config = {
        .resolver = resolve_policy,
        .resolver_user = &s_task_config,
    };
    assert(h2_esp_board_task_configure(&config) == s_task_configure_result);
    assert(s_task_configure_calls == 1u);
    assert(s_task_config.resolver == config.resolver);
    assert(s_task_config.resolver_user == config.resolver_user);
    assert(s_task_config.psram_stack_allocator == s_arena_mem);
    assert(config.psram_stack_allocator == NULL);
  }
  reset_stubs();
  s_task_configure_calls = 0u;
  const h2_esp_task_policy_config_t invalid = {0};
  assert(h2_esp_board_task_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_board_task_configure(&invalid) == H2_PAL_ERR_INVALID_ARG);
  assert(s_arena_init_calls == 0u && s_task_configure_calls == 0u);
}

int main(void) {
  test_arena_sizing();
  test_task_configure();
  test_deinit_is_a_noop_before_configuration();
  test_audio_failure_still_completes_teardown();
  test_first_error_wins_over_later_failures();
  test_later_failure_is_reported_when_audio_succeeds();
  test_fs_clear_routes_owned_paths();
  test_fs_clear_keeps_provider_after_reconfiguration();
  test_fs_prepare_mounts_owned_filesystems();
  test_fs_prepare_rejects_unowned_paths();
  test_fs_prepare_preserves_provider_and_reconfiguration();
  for (unsigned mode = 0u; mode < 2u; ++mode) {
    reset_stubs();
    s_mac_result = mode == 0u ? ESP_FAIL : ESP_OK;
    s_mac_zero = mode == 1u;
    h2_runtime_config_t config;
    assert(h2_esp_board_runtime_config(&config) == H2_PAL_OK);
    assert(config.ble_local_name == NULL);
    assert(h2_esp_board_runtime_deinit() == H2_PAL_OK);
  }
  return 0;
}

/* Reservation is exercised by the ESP PAL arena tests. */
void h2_esp_board_arena_init(void) { ++s_arena_init_calls; }
