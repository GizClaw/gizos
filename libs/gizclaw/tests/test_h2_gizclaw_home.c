#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition, const char *message) {
  if (condition)
    return 0;
  printf("FAIL home %s\n", message);
  return 1;
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
  uint8_t workflow_storage_bytes[4096];
  h2_gizclaw_resp_storage_t workflow_storage = {
      .data = workflow_storage_bytes,
      .capacity = sizeof(workflow_storage_bytes),
  };
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &workflow_storage, workflow_list, sizeof(workflow_list),
                      4u, &workflow_page) == H2_PAL_OK,
                  "workflow list decodes");
  fails += expect(
      workflow_page.count == 1u &&
          strcmp(workflow_page.items[0].collection, "assistants") == 0 &&
          strcmp(workflow_page.items[0].name, "story.aesop") == 0 &&
          workflow_page.items[0].i18n_count == 1u &&
          strcmp(workflow_page.items[0].i18n[0].locale, "zh-CN") == 0 &&
          strcmp(workflow_page.items[0].i18n[0].display_name, "全能对话") ==
              0 &&
          strcmp(workflow_page.items[0].i18n[0].description, "default") == 0 &&
          strcmp(workflow_page.runtime_profile_name, "demo") == 0 &&
          strcmp(workflow_page.runtime_profile_revision, "r1") == 0,
      "workflow projection preserves collection name i18n and revision");
  workflow_storage.used = 0u;

  uint8_t future_workflow_list[sizeof(workflow_list)];
  memcpy(future_workflow_list, workflow_list, sizeof(future_workflow_list));
  bool driver_replaced = false;
  for (size_t index = 0u; index + 1u < sizeof(future_workflow_list); ++index) {
    if (future_workflow_list[index] == 0x20u &&
        future_workflow_list[index + 1u] == 0x01u) {
      future_workflow_list[index + 1u] = 0x7fu;
      driver_replaced = true;
      break;
    }
  }
  fails += expect(driver_replaced, "workflow fixture driver is replaced");
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &workflow_storage, future_workflow_list,
                      sizeof(future_workflow_list), 4u,
                      &workflow_page) == H2_PAL_OK &&
                      workflow_page.count == 1u,
                  "workflow list ignores unknown driver metadata");
  workflow_storage.used = 0u;

  uint8_t invalid_workflow_list[sizeof(workflow_list)];
  memcpy(invalid_workflow_list, workflow_list, sizeof(invalid_workflow_list));
  memcpy(&invalid_workflow_list[4], "story..esop", 11u);
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &workflow_storage, invalid_workflow_list,
                      sizeof(invalid_workflow_list), 4u,
                      &workflow_page) == H2_PAL_ERR_FORMAT,
                  "workflow list rejects an empty alias segment");

  uint8_t embedded_nul_workflow_list[sizeof(workflow_list)];
  memcpy(embedded_nul_workflow_list, workflow_list,
         sizeof(embedded_nul_workflow_list));
  embedded_nul_workflow_list[4u + sizeof("story") - 1u] = '\0';
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &workflow_storage, embedded_nul_workflow_list,
                      sizeof(embedded_nul_workflow_list), 4u,
                      &workflow_page) == H2_PAL_ERR_FORMAT,
                  "workflow list rejects an embedded NUL alias byte");

  const uint8_t activation_payload[] = {
      0x0a, 0x1a, 0x0a, 0x07, 'w',  's', '-', 'c', 'h', 'a',
      't',  0x40, 0x03, 0x62, 0x04, 'c', 'h', 'a', 't', 0x6a,
      0x07, 'w',  's',  '-',  'c',  'h', 'a', 't',
  };
  uint8_t storage_buffer[4096];
  h2_gizclaw_resp_storage_t storage = {.data = storage_buffer,
                                       .capacity = sizeof(storage_buffer)};
  h2_gizclaw_workspace_activation_t activation = {0};
  fails += expect(h2_gizclaw_workspace_decode_activation_for_test(
                      &storage, activation_payload, sizeof(activation_payload),
                      &activation) == H2_PAL_OK,
                  "workspace activation decodes");
  fails +=
      expect(activation.runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
                 strcmp(activation.workspace_name, "ws-chat") == 0 &&
                 strcmp(activation.active_workspace_name, "ws-chat") == 0,
             "workspace commits only after matching running state");
  fails += expect(strcmp(activation.workspace_name, "ws-topic") != 0,
                  "workspace readiness rejects stale target");
  storage.used = 0u;

  const uint8_t history_list[] = {
      0x0a, 0x1e, 0x08, 0x01, 0x1a, 0x1a, 0x0a, 0x03, 'n',  'o',  'w',
      0x1a, 0x02, 'h',  '1',  0x22, 0x04, 'g',  'e',  'a',  'r',  0x28,
      0x01, 0x32, 0x05, 'h',  'e',  'l',  'l',  'o',  0x38, 0x01,
  };
  h2_gizclaw_workspace_history_page_t history_page = {0};
  fails += expect(h2_gizclaw_workspace_decode_history_list_for_test(
                      &storage, history_list, sizeof(history_list), 4u,
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
  storage.used = 0u;

  uint8_t invalid_history[sizeof(history_list)];
  memcpy(invalid_history, history_list, sizeof(invalid_history));
  invalid_history[sizeof(invalid_history) - 7u] = 0xffu;
  fails += expect(h2_gizclaw_workspace_decode_history_list_for_test(
                      &storage, invalid_history, sizeof(invalid_history), 4u,
                      &history_page) == H2_PAL_ERR_FORMAT,
                  "workspace history rejects invalid UTF-8");

  const uint8_t history_missing_cursor[] = {0x0a, 0x02, 0x10, 0x01};
  fails += expect(h2_gizclaw_workspace_decode_history_list_for_test(
                      &storage, history_missing_cursor,
                      sizeof(history_missing_cursor), 4u,
                      &history_page) == H2_PAL_ERR_FORMAT,
                  "workspace history rejects pagination without cursor");

  if (fails == 0)
    printf("PASS h2_gizclaw_home\n");
  return fails;
}
