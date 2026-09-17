#include "h2_loader_command.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct command_io_fixture {
  const char *input;
  size_t input_size;
  size_t input_offset;
  char output[512];
  size_t output_size;
  size_t flushes;
} command_io_fixture_t;

static h2_pal_result_t command_read(void *user, void *buffer, size_t len,
                                    size_t *out_read, uint32_t timeout_ms) {
  command_io_fixture_t *fixture = user;
  const size_t available = fixture->input_size - fixture->input_offset;
  const size_t take = len < available ? len : available;
  (void)timeout_ms;
  *out_read = take;
  if (take == 0u)
    return H2_PAL_ERR_TIMEOUT;
  memcpy(buffer, fixture->input + fixture->input_offset, take);
  fixture->input_offset += take;
  return H2_PAL_OK;
}

static h2_pal_result_t command_write(void *user, const void *buffer, size_t len,
                                     size_t *out_written, uint32_t timeout_ms) {
  command_io_fixture_t *fixture = user;
  (void)timeout_ms;
  assert(fixture->output_size + len <= sizeof(fixture->output));
  memcpy(fixture->output + fixture->output_size, buffer, len);
  fixture->output_size += len;
  *out_written = len;
  return H2_PAL_OK;
}

static h2_pal_result_t command_flush(void *user) {
  command_io_fixture_t *fixture = user;
  ++fixture->flushes;
  return H2_PAL_OK;
}

static int digest_start(void *user) {
  (void)user;
  return H2_PAL_OK;
}

static int digest_update(void *user, const uint8_t *data, size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_OK;
}

static int digest_finish(void *user, uint8_t out_digest[32]) {
  (void)user;
  memset(out_digest, 0, 32u);
  return H2_PAL_OK;
}

static uint64_t now_ms(void *user) {
  (void)user;
  return 0u;
}

static void sleep_ms(void *user, uint32_t delay_ms) {
  (void)user;
  (void)delay_ms;
}

static h2_pal_result_t memory_read(void *user,
                                   h2_loader_memory_stats_t *out_stats) {
  (void)user;
  memset(out_stats, 0, sizeof(*out_stats));
  return H2_PAL_OK;
}

static h2_pal_result_t command_init(h2_loader_command_t *command,
                                    h2_loader_t *loader,
                                    command_io_fixture_t *fixture) {
  static const h2_command_io_vtable_t io_vtable = {
      .read = command_read,
      .write = command_write,
      .flush = command_flush,
  };
  static const h2_pal_fs_vtable_t fs_vtable = {0};
  static const h2_pal_http_vtable_t http_vtable = {0};
  static const h2_pal_wifi_sta_vtable_t wifi_vtable = {0};
  static const h2_pal_disk_vtable_t disk_vtable = {0};
  static const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
  static const h2_pal_http_api_t http = {.vtable = &http_vtable};
  static const h2_pal_wifi_sta_api_t wifi = {.vtable = &wifi_vtable};
  static const h2_pal_disk_api_t disk = {.vtable = &disk_vtable};
  const h2_loader_command_config_t config = {
      .loader = loader,
      .fs = &fs,
      .http = &http,
      .wifi = &wifi,
      .disk = &disk,
      .digest =
          {
              .start = digest_start,
              .update = digest_update,
              .finish = digest_finish,
          },
      .memory_stats = {.read = memory_read},
      .now_ms = now_ms,
      .sleep_ms = sleep_ms,
      .io =
          {
              .user = fixture,
              .vtable = &io_vtable,
          },
  };
  return (h2_pal_result_t)h2_loader_command_init(command, &config);
}

static void test_exact_v2_routes_and_help(void) {
  static const char input[] = "h2loader help\nh2loader\n";
  static const char expected[] =
      "h2loader <help|status|stats|memory|wifi|stage|reboot "
      "app|loader|upgrade|coredump>\n"
      "usage: h2loader <help|status|stats|memory|wifi|stage|reboot "
      "app|loader|upgrade|coredump>\n";
  static const char *const paths[] = {
      "h2loader",       "h2loader help",   "h2loader status",
      "h2loader stats", "h2loader memory", "h2loader wifi",
      "h2loader stage", "h2loader reboot", "h2loader coredump",
  };
  command_io_fixture_t fixture = {
      .input = input,
      .input_size = sizeof(input) - 1u,
  };
  h2_loader_t loader = {0};
  h2_loader_command_t command;
  assert(command_init(&command, &loader, &fixture) == H2_PAL_OK);
  assert(command.command.definition_count ==
         H2_LOADER_COMMAND_DEFINITION_CAPACITY);
  for (size_t index = 0u; index < sizeof(paths) / sizeof(paths[0]); ++index) {
    assert(strcmp(command.definitions[index].path, paths[index]) == 0);
  }
  assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
  assert(h2_loader_command_poll(&command, 10u) == H2_PAL_OK);
  assert(fixture.flushes == 2u);
  assert(fixture.output_size == sizeof(expected) - 1u);
  assert(memcmp(fixture.output, expected, sizeof(expected) - 1u) == 0);
  const h2_loader_status_t status = {
      .capabilities = H2_LOADER_CAPABILITIES_ALL,
      .partition_2 = {
          .valid = 1,
          .role = H2_LOADER_IMAGE_ROLE_APP,
      },
  };
  assert(h2_loader_get_command_availability(&loader, &status) ==
         H2_LOADER_COMMAND_AVAILABILITY_ALL);
}

static void test_removed_commands_are_unroutable(void) {
  static const char *const removed[][4] = {
      {"h2loader", "restart", NULL, NULL},
      {"h2loader", "rollback", NULL, NULL},
      {"h2loader", "reboot", NULL, NULL},
      {"h2loader", "reboot", "ota", NULL},
      {"h2loader", "reboot-loader", NULL, NULL},
      {"h2loader", "upgrade", NULL, NULL},
      {"h2loader", "hold", "on", NULL},
      {"h2loader", "hold", "off", NULL},
  };
  static const size_t argc[] = {2u, 2u, 2u, 3u, 2u, 2u, 3u, 3u};
  command_io_fixture_t fixture = {0};
  h2_loader_t loader = {0};
  h2_loader_command_t command;
  assert(command_init(&command, &loader, &fixture) == H2_PAL_OK);
  for (size_t index = 0u; index < sizeof(argc) / sizeof(argc[0]); ++index) {
    assert(h2_loader_command_execute(&command, argc[index], removed[index]) !=
           H2_PAL_OK);
  }
}

static void test_command_availability_is_runtime_and_capability_bounded(void) {
  h2_loader_t loader = {0};
  h2_loader_status_t status = {
      .capabilities = H2_LOADER_CAPABILITIES_ALL,
  };
  const uint32_t core = H2_LOADER_COMMAND_AVAILABLE_HELP |
                        H2_LOADER_COMMAND_AVAILABLE_STATUS;
  assert(h2_loader_set_implemented_commands(&loader, core) == H2_PAL_OK);
  assert(h2_loader_get_command_availability(&loader, &status) == core);

  assert(h2_loader_set_implemented_commands(
             &loader, H2_LOADER_COMMAND_AVAILABILITY_ALL) == H2_PAL_OK);
  assert(h2_loader_set_command_availability(
             &loader, H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE, false) ==
         H2_PAL_OK);
  assert((h2_loader_get_command_availability(&loader, &status) &
          H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE) == 0u);

  status.capabilities &= ~H2_LOADER_CAPABILITY_WIFI;
  const uint32_t available =
      h2_loader_get_command_availability(&loader, &status);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN) == 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT) == 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT) == 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_STAGE_URL) == 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD) != 0u);
}

typedef struct wifi_fixture {
  int result;
  int status_result, settings_result, get_result, saved, disconnected;
  unsigned saves, connects;
  size_t saved_ssid_len;
  h2_pal_wifi_sta_config_t target;
} wifi_fixture_t;

static int wifi_connect(void *user, const h2_pal_wifi_sta_config_t *config,
                        uint32_t timeout_ms) {
  wifi_fixture_t *f = user;
  assert(timeout_ms == 15000u);
  assert(f->saves == 0);
  ++f->connects;
  f->target = *config;
  return f->result;
}
static int wifi_status(void *user, h2_pal_wifi_sta_status_t *status) {
  wifi_fixture_t *f = user;
  memset(status, 0, sizeof(*status));
  status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  status->ip_valid = 1;
  status->ssid_len = f->target.ssid_len;
  memcpy(status->ssid, f->target.ssid, status->ssid_len);
  status->ip.ip4 = 0xc0000201u;
  status->rssi = -42;
  if (f->disconnected) {
    status->state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    status->ip_valid = 0;
    status->ssid_len = 0;
    status->disconnect_reason = 7;
  }
  return f->status_result;
}
static int wifi_connect_and_save(void *user, const h2_pal_wifi_sta_config_t *config,
                                 uint32_t timeout_ms) {
  wifi_fixture_t *f = user;
  int rc = wifi_connect(user, config, timeout_ms);
  if (rc == H2_PAL_OK)
    ++f->saves;
  return rc;
}
static void test_wifi_saves_only_after_connection(void) {
  command_io_fixture_t io = {0};
  h2_loader_t loader = {0};
  loader.status.capabilities = H2_LOADER_CAPABILITY_WIFI;
  h2_loader_command_t command;
  assert(command_init(&command, &loader, &io) == H2_PAL_OK);
  wifi_fixture_t f = {.result = H2_PAL_ERR_IO};
  const h2_pal_wifi_sta_vtable_t sta_vtable = {
    .connect_and_save = wifi_connect_and_save, .get_status = wifi_status};
  const h2_pal_wifi_sta_api_t sta = {&f, &sta_vtable};
  command.config.wifi = &sta;
  const char *args[] = {"h2loader", "wifi", "connect", "network", "password"};
  assert(h2_loader_command_execute(&command, 5, args) == H2_PAL_ERR_IO);
  assert(f.saves == 0);
  f.result = H2_PAL_OK;
  assert(h2_loader_command_execute(&command, 5, args) == H2_PAL_OK);
  assert(f.saves == 1);
  f.saves = 0;
  assert(h2_loader_command_execute(&command, 5, args) == H2_PAL_OK);
  assert(f.saves == 1); /* Exactly one persistent provider operation. */
}

static int wifi_has_saved(void *user, int *out) {
    wifi_fixture_t *f = user;
    *out = f->saved;
    return f->settings_result;
}

static int wifi_get_saved(void *user, h2_pal_wifi_sta_config_t *out) {
    wifi_fixture_t *f = user;
    *out = f->target;
    if (f->saved_ssid_len != 0u) out->ssid_len = f->saved_ssid_len;
    return f->get_result;
}

static unsigned status_locks, status_unlocks;
static h2_pal_result_t status_lock(void *user, h2_pal_mutex_t *mutex) {
    assert((void *)mutex == user); /* The Wi-Fi mutex is deliberately different. */
    ++status_locks;
    return H2_PAL_OK;
}

static h2_pal_result_t status_unlock(void *user, h2_pal_mutex_t *mutex) {
    assert((void *)mutex == user);
    ++status_unlocks;
    return H2_PAL_OK;
}

static void test_wifi_status_snapshot(void) {
    command_io_fixture_t io = {0};
    h2_loader_t loader = {0};
    loader.status.capabilities = H2_LOADER_CAPABILITY_WIFI;
    h2_loader_command_t command;
    assert(command_init(&command, &loader, &io) == H2_PAL_OK);
    wifi_fixture_t f = {.saved = 1, .target = {
        .ssid = "a b", .ssid_len = 3, .password = "placeholder", .password_len = 11}};
    const h2_pal_wifi_sta_vtable_t sta_vtable = {.get_status = wifi_status};
    const h2_pal_wifi_sta_api_t sta = {&f, &sta_vtable};
    const h2_pal_wifi_settings_vtable_t settings_vtable = {
        .has_saved_sta_config = wifi_has_saved, .get_saved_sta_config = wifi_get_saved};
    const h2_pal_wifi_settings_api_t settings = {&f, &settings_vtable};
    command.config.wifi = &sta;
    command.config.wifi_settings = &settings;
    const h2_pal_sync_vtable_t sync_vtable = {
        .lock_mutex = status_lock, .unlock_mutex = status_unlock};
    const h2_pal_sync_api_t sync = {.user = &f, .vtable = &sync_vtable};
    command.config.operation_sync = &sync;
    command.config.operation_mutex = (h2_pal_mutex_t *)&f;
    command.config.wifi_operation_sync = &sync;
    command.config.wifi_operation_mutex = (h2_pal_mutex_t *)&io;
    const char *args[] = {"h2loader", "wifi", "status"};
    assert(H2_LOADER_COMMAND_AVAILABLE_WIFI_STATUS == (1u << 20));
    assert(h2_loader_get_command_availability(&loader, &loader.status) &
        H2_LOADER_COMMAND_AVAILABLE_WIFI_STATUS);
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_OK);
    assert(strcmp(io.output, "H2_LOADER_WIFI_STATUS result=OK state=5 ip_valid=1 "
        "ip=192.0.2.1 ssid_hex=612062 rssi=-42 disconnect_reason=0 "
        "saved=1 saved_code=0 saved_ssid_hex=612062\n") == 0);
    assert(strstr(io.output, "placeholder") == NULL);
    assert(status_locks == 1 && status_unlocks == 1);
    memset(&io, 0, sizeof(io));
    f.target.ssid_len = 33u;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_ERR_FORMAT);
    assert(strcmp(io.output,
        "H2_LOADER_WIFI_STATUS result=error code=-15\n") == 0);
    memset(&io, 0, sizeof(io));
    f.target.ssid_len = 3u;
    f.saved_ssid_len = 33u;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_OK);
    assert(strcmp(io.output, "H2_LOADER_WIFI_STATUS result=OK state=5 ip_valid=1 "
        "ip=192.0.2.1 ssid_hex=612062 rssi=-42 disconnect_reason=0 "
        "saved=error saved_code=-15 saved_ssid_hex=-\n") == 0);
    assert(strstr(io.output, "placeholder") == NULL);
    f.saved_ssid_len = 0u;
    memset(&io, 0, sizeof(io));
    f.saved = 0;
    f.disconnected = 1;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_OK);
    assert(strstr(io.output, "saved=0 saved_code=0 saved_ssid_hex=-\n"));
    assert(strstr(io.output, "ip_valid=0 ip=0.0.0.0 ssid_hex=- rssi=-42 disconnect_reason=7"));
    memset(&io, 0, sizeof(io));
    f.disconnected = 0;
    f.settings_result = H2_PAL_ERR_IO;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_OK);
    assert(strstr(io.output, "result=OK state=5"));
    char expected[100];
    snprintf(expected, sizeof(expected), "saved=error saved_code=%d saved_ssid_hex=-\n", H2_PAL_ERR_IO);
    assert(strstr(io.output, expected));
    memset(&io, 0, sizeof(io));
    f.settings_result = 0;
    f.saved = 1;
    f.get_result = H2_PAL_ERR_IO;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_OK);
    assert(strstr(io.output, expected));
    memset(&io, 0, sizeof(io));
    f.status_result = H2_PAL_ERR_IO;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_ERR_IO);
    snprintf(expected, sizeof(expected), "H2_LOADER_WIFI_STATUS result=error code=%d\n", H2_PAL_ERR_IO);
    assert(strcmp(io.output, expected) == 0);
    memset(&io, 0, sizeof(io));
    loader.status.capabilities = H2_LOADER_CAPABILITY_UART;
    assert(!(h2_loader_get_command_availability(&loader, &loader.status) &
        H2_LOADER_COMMAND_AVAILABLE_WIFI_STATUS));
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_loader_set_command_availability(&loader,
        H2_LOADER_COMMAND_AVAILABLE_WIFI_STATUS, false) == H2_PAL_OK);
    loader.status.capabilities |= H2_LOADER_CAPABILITY_WIFI;
    assert(h2_loader_command_execute(&command, 3, args) == H2_PAL_ERR_INVALID_STATE);
}

int main(void) {
  test_wifi_status_snapshot();
  test_wifi_saves_only_after_connection();
  test_exact_v2_routes_and_help();
  test_removed_commands_are_unroutable();
  test_command_availability_is_runtime_and_capability_bounded();
  puts("h2loader v2 command tests passed");
  return 0;
}
