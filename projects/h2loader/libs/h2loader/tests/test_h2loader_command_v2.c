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

int main(void) {
  test_exact_v2_routes_and_help();
  test_removed_commands_are_unroutable();
  test_command_availability_is_runtime_and_capability_bounded();
  puts("h2loader v2 command tests passed");
  return 0;
}
