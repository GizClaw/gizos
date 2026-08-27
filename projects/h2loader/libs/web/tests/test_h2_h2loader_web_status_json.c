#include "h2_h2loader_web_status_json.h"

#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RUNTIME_STATUS_CAPACITY \
  (12288u - (6u * (256u - 1u) + 64u))

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *pointer, size_t len) {
  (void)user;
  return realloc(pointer, len);
}

static void test_free(void *user, void *pointer) {
  (void)user;
  free(pointer);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_mem_api_t test_mem = {
    .vtable = &test_mem_vtable,
};

static h2_h2loader_host_status_t test_status(void) {
  h2_h2loader_host_status_t status = {
      .active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
      .capabilities = H2_H2LOADER_HOST_CAP_STATUS |
                      H2_H2LOADER_HOST_CAP_REBOOT,
      .staged_bytes = 42u,
      .staged_valid = 1u,
      .installed_valid = 1u,
      .app_confirmed = 0u,
  };
  (void)snprintf(status.board, sizeof(status.board), "bo\"ard\\\n");
  (void)snprintf(status.target, sizeof(status.target), "target");
  (void)snprintf(status.chip, sizeof(status.chip), "chip");
  (void)snprintf(status.active_name, sizeof(status.active_name), "loader");
  (void)snprintf(status.active_version, sizeof(status.active_version), "1.2.3");
  (void)snprintf(status.active_checksum, sizeof(status.active_checksum), "aa");
  (void)snprintf(status.installed_checksum,
                 sizeof(status.installed_checksum), "bb");
  (void)snprintf(status.state, sizeof(status.state), "confirmed");
  (void)snprintf(status.upgrade_phase, sizeof(status.upgrade_phase), "idle");
  return status;
}

static h2_pal_json_document_t *parse_json(
    h2_yyjson_json_t *provider, const char *json, size_t size,
    h2_pal_json_value_t **out_root) {
  h2_pal_json_document_t *document = NULL;
  assert(h2_pal_json_document_parse(
             h2_yyjson_json_api(provider), (const uint8_t *)json, size, NULL,
             &document) == H2_PAL_OK);
  assert(h2_pal_json_document_root(
             h2_yyjson_json_api(provider), document, out_root) == H2_PAL_OK);
  h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
  assert(h2_pal_json_value_type(
             h2_yyjson_json_api(provider), *out_root, &type) == H2_PAL_OK);
  assert(type == H2_PAL_JSON_TYPE_OBJECT);
  return document;
}

static void assert_command_availability(
    h2_yyjson_json_t *provider, const h2_h2loader_host_status_t *status,
    int expected_present, uint32_t expected_value) {
  char json[TEST_RUNTIME_STATUS_CAPACITY];
  size_t size = 0u;
  assert(h2_h2loader_web_status_json_write(
             status, json, sizeof(json), &size) == H2_PAL_OK);
  assert(size == strlen(json));

  h2_pal_json_value_t *root = NULL;
  h2_pal_json_document_t *document =
      parse_json(provider, json, size, &root);
  h2_pal_json_value_t *availability = NULL;
  const h2_pal_result_t result = h2_pal_json_object_get(
      h2_yyjson_json_api(provider), root, "commandAvailability",
      strlen("commandAvailability"), &availability);
  if (expected_present) {
    assert(result == H2_PAL_OK);
    double value = 0.0;
    assert(h2_pal_json_value_get_number(
               h2_yyjson_json_api(provider), availability, &value) ==
           H2_PAL_OK);
    assert(value == (double)expected_value);
  } else {
    assert(result == H2_PAL_ERR_NOT_FOUND);
    assert(availability == NULL);
  }

  h2_pal_json_value_t *board = NULL;
  assert(h2_pal_json_object_get(h2_yyjson_json_api(provider), root, "board",
                                strlen("board"), &board) == H2_PAL_OK);
  h2_pal_json_string_view_t board_value = {0};
  assert(h2_pal_json_value_get_string(
             h2_yyjson_json_api(provider), board, &board_value) == H2_PAL_OK);
  assert(board_value.len == strlen(status->board));
  assert(memcmp(board_value.data, status->board, board_value.len) == 0);
  assert(h2_pal_json_document_destroy(
             h2_yyjson_json_api(provider), &document) == H2_PAL_OK);
}

static void fill_string(char *value, size_t capacity, char byte) {
  assert(capacity > 0u);
  memset(value, byte, capacity - 1u);
  value[capacity - 1u] = '\0';
}

static void test_maximum_projection_fits(h2_yyjson_json_t *provider) {
  h2_h2loader_host_status_t status = test_status();
  fill_string(status.board, sizeof(status.board), 0x1f);
  fill_string(status.target, sizeof(status.target), 0x1f);
  fill_string(status.chip, sizeof(status.chip), 0x1f);
  fill_string(status.active_name, sizeof(status.active_name), 0x1f);
  fill_string(status.active_version, sizeof(status.active_version), 0x1f);
  fill_string(status.active_checksum, sizeof(status.active_checksum), 0x1f);
  fill_string(status.installed_checksum, sizeof(status.installed_checksum),
              0x1f);
  fill_string(status.state, sizeof(status.state), 0x1f);
  fill_string(status.upgrade_phase, sizeof(status.upgrade_phase), 0x1f);
  status.has_command_availability = 1u;
  status.command_availability = UINT32_MAX;
  assert_command_availability(provider, &status, 1, UINT32_MAX);
}

static void test_truncation(void) {
  h2_h2loader_host_status_t status = test_status();
  char json[16];
  memset(json, 'x', sizeof(json));
  size_t size = 123u;
  assert(h2_h2loader_web_status_json_write(
             &status, json, sizeof(json), &size) == H2_PAL_ERR_NO_SPACE);
  assert(size == 0u);
  assert(json[0] == '\0');
}

int main(void) {
  h2_yyjson_json_t *provider = NULL;
  assert(h2_yyjson_json_create(&test_mem, &provider) == H2_PAL_OK);

  h2_h2loader_host_status_t status = test_status();
  status.command_availability = UINT32_MAX;
  assert_command_availability(provider, &status, 0, 0u);

  static const uint32_t masks[] = {
      0u,
      H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP,
      H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER,
      H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP |
          H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER,
      UINT32_C(0x80000003),
  };
  status.has_command_availability = 1u;
  for (size_t index = 0u; index < sizeof(masks) / sizeof(masks[0]); ++index) {
    status.command_availability = masks[index];
    assert_command_availability(provider, &status, 1, masks[index]);
  }

  test_maximum_projection_fits(provider);
  test_truncation();
  assert(h2_yyjson_json_destroy(&provider) == H2_PAL_OK);
  return 0;
}
