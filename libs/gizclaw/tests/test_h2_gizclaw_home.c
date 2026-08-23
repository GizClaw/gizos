#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t test_time(void *user, uint64_t *out_ms) {
  (void)user;
  *out_ms = 1234u;
  return H2_PAL_OK;
}

static int expect(int condition, const char *message) {
  if (condition)
    return 0;
  printf("FAIL home %s\n", message);
  return 1;
}

typedef struct alias_rpc_mock {
  size_t calls;
  h2_gizclaw_rpc_method_t last_method;
  bool request_has_dotted_alias;
} alias_rpc_mock_t;

static bool bytes_contains(const uint8_t *data, size_t len, const char *text) {
  const size_t text_len = strlen(text);
  if (data == NULL || text_len > len)
    return false;
  for (size_t index = 0u; index + text_len <= len; ++index) {
    if (memcmp(data + index, text, text_len) == 0)
      return true;
  }
  return false;
}

static int alias_rpc_call(void *user, h2_gizclaw_client_t *client,
                          h2_gizclaw_rpc_method_t method,
                          h2_gizclaw_rpc_bytes_t params_payload,
                          h2_gizclaw_rpc_response_t *out_response) {
  alias_rpc_mock_t *mock = user;
  (void)client;
  (void)out_response;
  ++mock->calls;
  mock->last_method = method;
  mock->request_has_dotted_alias =
      bytes_contains(params_payload.data, params_payload.len, "story.aesop");
  return H2_PAL_ERR_IO;
}

int h2_gizclaw_home_tests(void) {
  int fails = 0;
  static const char *const valid_aliases[] = {
      "chat",
      "story.aesop",
      "story.journey.center-earth",
      "adventure.debate",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  };
  for (size_t index = 0u;
       index < sizeof(valid_aliases) / sizeof(valid_aliases[0]); ++index) {
    const char *alias = valid_aliases[index];
    fails += expect(h2_gizclaw_runtime_alias_valid_internal((h2_gizclaw_str_t){
                        .data = alias, .len = strlen(alias)}),
                    "canonical RuntimeProfile alias is valid");
  }
  static const char *const invalid_aliases[] = {
      "",
      ".voice",
      "raid.",
      "raid..voice",
      "raid.-voice",
      "raid-.voice",
      "raid--voice",
      "Story.journey",
      "story_journey",
      "story/journey",
      " story.journey",
      "story. journey",
      "story.journey ",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  };
  for (size_t index = 0u;
       index < sizeof(invalid_aliases) / sizeof(invalid_aliases[0]); ++index) {
    const char *alias = invalid_aliases[index];
    fails += expect(!h2_gizclaw_runtime_alias_valid_internal((h2_gizclaw_str_t){
                        .data = alias, .len = strlen(alias)}),
                    "malformed RuntimeProfile alias is invalid");
  }
  static const char embedded_nul_alias[] = {'a', '\0', 'b'};
  fails += expect(
      !h2_gizclaw_runtime_alias_valid_internal((h2_gizclaw_str_t){
          .data = embedded_nul_alias, .len = sizeof(embedded_nul_alias)}),
      "RuntimeProfile alias rejects embedded NUL");
  const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  const h2_pal_mem_api_t mem = {.user = NULL, .vtable = &mem_vtable};
  const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time,
  };
  const h2_pal_time_api_t time = {.user = NULL, .vtable = &time_vtable};
  const h2_pal_http_api_t http = {0};
  const h2_pal_webrtc_api_t webrtc = {0};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_gizclaw_config_t config = {
      .server_endpoint = {.data = "127.0.0.1:19820", .len = 15u},
      .private_key = {.data = "test-private-key", .len = 16u},
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = 1000,
      .allocator = &mem,
      .http = &http,
      .webrtc = &webrtc,
      .crypto = &crypto,
      .time = &time,
  };
  h2_gizclaw_client_t *client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK,
                  "client initializes");

  alias_rpc_mock_t alias_rpc = {0};
  h2_gizclaw_test_set_rpc_call(alias_rpc_call, &alias_rpc);
  h2_gizclaw_workflow_t dotted_workflow = {0};
  char *profile_name = NULL;
  char *profile_revision = NULL;
  fails +=
      expect(h2_gizclaw_client_workflow_get(
                 client, (h2_gizclaw_str_t){.data = "story.aesop", .len = 11u},
                 &dotted_workflow, &profile_name,
                 &profile_revision) == H2_PAL_ERR_IO &&
                 alias_rpc.calls == 1u && alias_rpc.request_has_dotted_alias &&
                 alias_rpc.last_method == H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET,
             "workflow get accepts a dotted RuntimeProfile alias");
  h2_gizclaw_workflow_get_deinit(client, &dotted_workflow, profile_name,
                                 profile_revision);
  fails +=
      expect(h2_gizclaw_client_workflow_get(
                 client, (h2_gizclaw_str_t){.data = "story..esop", .len = 11u},
                 &dotted_workflow, &profile_name,
                 &profile_revision) == H2_PAL_ERR_INVALID_ARG &&
                 alias_rpc.calls == 1u,
             "workflow get rejects a malformed RuntimeProfile alias");

  h2_gizclaw_workspace_t dotted_workspace = {0};
  fails += expect(
      h2_gizclaw_client_workspace_create(
          client, (h2_gizclaw_str_t){.data = "story-teller", .len = 12u},
          (h2_gizclaw_str_t){.data = "story.aesop", .len = 11u},
          (h2_gizclaw_str_t){.data = "demo-story.aesop", .len = 16u},
          &dotted_workspace) == H2_PAL_ERR_IO &&
          alias_rpc.calls == 2u && alias_rpc.request_has_dotted_alias &&
          alias_rpc.last_method == H2_GIZCLAW_RPC_SERVER_WORKSPACE_CREATE,
      "workspace create accepts a dotted RuntimeProfile alias");
  h2_gizclaw_workspace_deinit(client, &dotted_workspace);
  fails +=
      expect(h2_gizclaw_client_workspace_create(
                 client, (h2_gizclaw_str_t){.data = "story-teller", .len = 12u},
                 (h2_gizclaw_str_t){.data = "story..esop", .len = 11u},
                 (h2_gizclaw_str_t){.data = "demo-story.aesop", .len = 16u},
                 &dotted_workspace) == H2_PAL_ERR_INVALID_ARG &&
                 alias_rpc.calls == 2u,
             "workspace create rejects a malformed RuntimeProfile alias");
  h2_gizclaw_test_set_rpc_call(NULL, NULL);

  const uint8_t workflow_list[] = {
      0x12, 0x3d, 0x0a, 0x0b, 's',  't',  'o',  'r',  'y',  '.',  'a',
      'e',  's',  'o',  'p',  0x12, 0x20, 0x0a, 0x05, 'z',  'h',  '-',
      'C',  'N',  0x12, 0x17, 0x0a, 0x0c, 0xe5, 0x85, 0xa8, 0xe8, 0x83,
      0xbd, 0xe5, 0xaf, 0xb9, 0xe8, 0xaf, 0x9d, 0x12, 0x07, 'd',  'e',
      'f',  'a',  'u',  'l',  't',  0x1a, 0x0a, 'a',  's',  's',  'i',
      's',  't',  'a',  'n',  't',  's',  0x20, 0x01, 0x22, 0x04, 'd',
      'e',  'm',  'o',  0x2a, 0x02, 'r',  '1',
  };
  h2_gizclaw_workflow_page_t workflow_page = {0};
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      client, workflow_list, sizeof(workflow_list), 4u,
                      &workflow_page) == H2_PAL_OK,
                  "workflow list decodes");
  fails +=
      expect(workflow_page.count == 1u &&
                 strcmp(workflow_page.items[0].collection, "assistants") == 0 &&
                 strcmp(workflow_page.items[0].name, "story.aesop") == 0 &&
                 workflow_page.items[0].driver ==
                     H2_GIZCLAW_WORKFLOW_DRIVER_FLOWCRAFT &&
                 strcmp(h2_gizclaw_workflow_display_name(
                            &workflow_page.items[0], "zh-CN"),
                        "全能对话") == 0 &&
                 strcmp(h2_gizclaw_workflow_description(&workflow_page.items[0],
                                                        "zh-CN"),
                        "default") == 0 &&
                 strcmp(workflow_page.runtime_profile_name, "demo") == 0 &&
                 strcmp(workflow_page.runtime_profile_revision, "r1") == 0,
             "workflow projection preserves collection name i18n and revision");
  h2_gizclaw_workflow_page_deinit(client, &workflow_page);

  uint8_t invalid_workflow_list[sizeof(workflow_list)];
  memcpy(invalid_workflow_list, workflow_list, sizeof(invalid_workflow_list));
  memcpy(&invalid_workflow_list[4], "story..esop", 11u);
  fails +=
      expect(h2_gizclaw_workflow_decode_list_for_test(
                 client, invalid_workflow_list, sizeof(invalid_workflow_list),
                 4u, &workflow_page) == H2_PAL_ERR_FORMAT,
             "workflow list rejects an empty alias segment");

  uint8_t embedded_nul_workflow_list[sizeof(workflow_list)];
  memcpy(embedded_nul_workflow_list, workflow_list,
         sizeof(embedded_nul_workflow_list));
  embedded_nul_workflow_list[4u + sizeof("story") - 1u] = '\0';
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      client, embedded_nul_workflow_list,
                      sizeof(embedded_nul_workflow_list), 4u,
                      &workflow_page) == H2_PAL_ERR_FORMAT,
                  "workflow list rejects an embedded NUL alias byte");

  const uint8_t activation_payload[] = {
      0x0a, 0x1a, 0x0a, 0x07, 'w',  's', '-', 'c', 'h', 'a',
      't',  0x40, 0x03, 0x62, 0x04, 'c', 'h', 'a', 't', 0x6a,
      0x07, 'w',  's',  '-',  'c',  'h', 'a', 't',
  };
  h2_gizclaw_workspace_activation_t activation = {0};
  fails += expect(h2_gizclaw_workspace_decode_activation_for_test(
                      client, activation_payload, sizeof(activation_payload),
                      &activation) == H2_PAL_OK,
                  "workspace activation decodes");
  fails += expect(h2_gizclaw_workspace_activation_ready(&activation, "ws-chat"),
                  "workspace commits only after matching running state");
  fails +=
      expect(!h2_gizclaw_workspace_activation_ready(&activation, "ws-topic"),
             "workspace readiness rejects stale target");
  h2_gizclaw_workspace_activation_deinit(client, &activation);

  const uint8_t history_list[] = {
      0x0a, 0x1e, 0x08, 0x01, 0x1a, 0x1a, 0x0a, 0x03, 'n',  'o',  'w',
      0x1a, 0x02, 'h',  '1',  0x22, 0x04, 'g',  'e',  'a',  'r',  0x28,
      0x01, 0x32, 0x05, 'h',  'e',  'l',  'l',  'o',  0x38, 0x01,
  };
  h2_gizclaw_workspace_history_page_t history_page = {0};
  fails += expect(h2_gizclaw_workspace_decode_history_list_for_test(
                      client, history_list, sizeof(history_list), 4u,
                      &history_page) == H2_PAL_OK,
                  "workspace history list decodes");
  fails += expect(
      history_page.available && history_page.count == 1u &&
          strcmp(history_page.items[0].id, "h1") == 0 &&
          strcmp(history_page.items[0].created_at, "now") == 0 &&
          strcmp(history_page.items[0].name, "gear") == 0 &&
          strcmp(history_page.items[0].text, "hello") == 0 &&
          history_page.items[0].replay_available &&
          history_page.items[0].type == H2_GIZCLAW_WORKSPACE_HISTORY_GEAR,
      "workspace history preserves stable identity and replay metadata");
  h2_gizclaw_workspace_history_page_deinit(client, &history_page);

  uint8_t invalid_history[sizeof(history_list)];
  memcpy(invalid_history, history_list, sizeof(invalid_history));
  invalid_history[sizeof(invalid_history) - 7u] = 0xffu;
  fails += expect(h2_gizclaw_workspace_decode_history_list_for_test(
                      client, invalid_history, sizeof(invalid_history), 4u,
                      &history_page) == H2_PAL_ERR_FORMAT,
                  "workspace history rejects invalid UTF-8");

  const uint8_t history_missing_cursor[] = {0x0a, 0x02, 0x10, 0x01};
  fails +=
      expect(h2_gizclaw_workspace_decode_history_list_for_test(
                 client, history_missing_cursor, sizeof(history_missing_cursor),
                 4u, &history_page) == H2_PAL_ERR_FORMAT,
             "workspace history rejects pagination without cursor");

  h2_gizclaw_client_deinit(client);
  if (fails == 0)
    printf("PASS h2_gizclaw_home\n");
  return fails;
}
