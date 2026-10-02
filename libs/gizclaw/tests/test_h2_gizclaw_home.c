#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#include "payload/ai.pb.h"
#include "pb_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition, const char *message) {
  if (condition)
    return 0;
  printf("FAIL home %s\n", message);
  return 1;
}

typedef struct workflow_tag_fixture {
  const h2_gizclaw_str_t *tags;
  size_t count;
} workflow_tag_fixture_t;
static bool tag_test_text(pb_ostream_t *stream, const pb_field_t *field,
                          void *const *arg) {
  const char *text = *arg;
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text, strlen(text));
}
static bool tag_test_tags(pb_ostream_t *stream, const pb_field_t *field,
                          void *const *arg) {
  const workflow_tag_fixture_t *fixture = *arg;
  for (size_t i = 0; i < fixture->count; ++i)
    if (!pb_encode_tag_for_field(stream, field) ||
        !pb_encode_string(stream, (const pb_byte_t *)fixture->tags[i].data,
                          fixture->tags[i].len))
      return false;
  return true;
}
static bool tag_test_item(pb_ostream_t *stream, const pb_field_t *field,
                          void *const *arg) {
  gizclaw_rpc_v1_Workflow workflow = gizclaw_rpc_v1_Workflow_init_zero;
  workflow.name =
      (pb_callback_t){.funcs.encode = tag_test_text, .arg = "story.aesop"};
  workflow.tags = (pb_callback_t){.funcs.encode = tag_test_tags, .arg = *arg};
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_submessage(stream, gizclaw_rpc_v1_Workflow_fields,
                              &workflow);
}
typedef struct safety_options_fixture {
  const char *names[2];
  const char *labels[2];
  size_t count;
} safety_options_fixture_t;
static bool encode_safety_options(pb_ostream_t *stream,
                                  const pb_field_t *field, void *const *arg) {
  const safety_options_fixture_t *fixture = *arg;
  for (size_t i = 0u; i < fixture->count; ++i) {
    gizclaw_rpc_v1_SafetyFenceOption option =
        gizclaw_rpc_v1_SafetyFenceOption_init_zero;
    strcpy(option.name, fixture->names[i]);
    option.has_display_name = fixture->labels[i] != NULL;
    if (option.has_display_name)
      strcpy(option.display_name, fixture->labels[i]);
    if (!pb_encode_tag_for_field(stream, field) ||
        !pb_encode_submessage(stream, gizclaw_rpc_v1_SafetyFenceOption_fields,
                              &option))
      return false;
  }
  return true;
}
static int test_safety_options(void) {
  int fails = 0;
  safety_options_fixture_t fixture = {
      .names = {"safe", "strict_v2"},
      .labels = {"Safe", "Strict"},
      .count = 2u};
  gizclaw_rpc_v1_WorkflowListResponse message =
      gizclaw_rpc_v1_WorkflowListResponse_init_zero;
  message.runtime_profile_name =
      (pb_callback_t){.funcs.encode = tag_test_text, .arg = "profile"};
  message.runtime_profile_revision =
      (pb_callback_t){.funcs.encode = tag_test_text, .arg = "revision"};
  message.safety_fences = (pb_callback_t){.funcs.encode = encode_safety_options,
                                          .arg = &fixture};
  uint8_t wire[512], buffer[2048];
  pb_ostream_t output = pb_ostream_from_buffer(wire, sizeof(wire));
  fails += expect(pb_encode(&output, gizclaw_rpc_v1_WorkflowListResponse_fields,
                            &message), "safety options encode");
  h2_gizclaw_resp_storage_t storage = {buffer, sizeof(buffer), 0u};
  h2_gizclaw_workflow_page_t page = {0};
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &storage, wire, output.bytes_written, 1u, &page) ==
                          H2_PAL_OK &&
                      page.safety_fence_count == 2u &&
                      strcmp(page.safety_fences[0].name, "safe") == 0 &&
                      page.safety_fences[0].has_display_name &&
                      strcmp(page.safety_fences[1].display_name, "Strict") == 0,
                  "Profile safety options decode without prompt text");
  fixture.names[1] = "safe";
  storage.used = 0u;
  output = pb_ostream_from_buffer(wire, sizeof(wire));
  fails += expect(pb_encode(&output, gizclaw_rpc_v1_WorkflowListResponse_fields,
                            &message), "duplicate safety options encode");
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &storage, wire, output.bytes_written, 1u, &page) ==
                          H2_PAL_ERR_FORMAT &&
                      storage.used == 0u,
                  "duplicate safety options reject atomically");
  fixture.names[1] = "Invalid";
  output = pb_ostream_from_buffer(wire, sizeof(wire));
  fails += expect(pb_encode(&output, gizclaw_rpc_v1_WorkflowListResponse_fields,
                            &message), "invalid safety option encode");
  fails += expect(h2_gizclaw_workflow_decode_list_for_test(
                      &storage, wire, output.bytes_written, 1u, &page) ==
                          H2_PAL_ERR_FORMAT,
                  "invalid safety option rejects before display");
  return fails;
}
static int test_workflow_tags(void) {
  int fails = 0;
  char names[33][16];
  h2_gizclaw_str_t tags[33];
  for (size_t i = 0; i < 33; ++i) {
    (void)snprintf(names[i], sizeof(names[i]), "tag-%zu", i);
    tags[i] = (h2_gizclaw_str_t){names[i], strlen(names[i])};
  }
  for (size_t scenario = 0; scenario < 10; ++scenario) {
    workflow_tag_fixture_t fixture = {tags, scenario == 0   ? 0u
                                            : scenario == 1 ? 32u
                                                            : 1u};
    const char bad[] = {(char)0xff};
    char long_tag[129];
    memset(long_tag, 'x', sizeof(long_tag));
    h2_gizclaw_str_t one = {"Tag: \xe9\x98\x85\xe8\xaf\xbb ",
                            strlen("Tag: \xe9\x98\x85\xe8\xaf\xbb ")};
    if (scenario >= 2)
      fixture.tags = &one;
    if (scenario == 3)
      one = (h2_gizclaw_str_t){"", 0u};
    if (scenario == 4)
      one = (h2_gizclaw_str_t){bad, sizeof(bad)};
    if (scenario == 5)
      one = (h2_gizclaw_str_t){long_tag, sizeof(long_tag)};
    if (scenario == 6)
      fixture = (workflow_tag_fixture_t){tags, 33u};
    h2_gizclaw_str_t duplicated[] = {{"same", 4u}, {"same", 4u}};
    if (scenario == 7)
      fixture = (workflow_tag_fixture_t){duplicated, 2u};
    if (scenario == 8)
      one = (h2_gizclaw_str_t){"a\0b", 3u};
    if (scenario == 9)
      one = (h2_gizclaw_str_t){long_tag, 128u};
    gizclaw_rpc_v1_WorkflowListResponse message =
        gizclaw_rpc_v1_WorkflowListResponse_init_zero;
    message.items =
        (pb_callback_t){.funcs.encode = tag_test_item, .arg = &fixture};
    message.runtime_profile_name =
        (pb_callback_t){.funcs.encode = tag_test_text, .arg = "profile"};
    message.runtime_profile_revision =
        (pb_callback_t){.funcs.encode = tag_test_text, .arg = "revision"};
    uint8_t encoded[2048], bytes[16384];
    pb_ostream_t output = pb_ostream_from_buffer(encoded, sizeof(encoded));
    if (!pb_encode(&output, gizclaw_rpc_v1_WorkflowListResponse_fields,
                   &message))
      return 1;
    h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
    h2_gizclaw_workflow_page_t page = {0};
    int rc = h2_gizclaw_workflow_decode_list_for_test(
        &storage, encoded, output.bytes_written, 1u, &page);
    if (scenario >= 3 && scenario != 9)
      fails += expect(rc == H2_PAL_ERR_FORMAT && storage.used == 0u,
                      "invalid Workflow tags roll back response storage");
    else {
      fails += expect(rc == H2_PAL_OK && page.count == 1u &&
                          page.items[0].tag_count == fixture.count,
                      "Workflow tags decode at zero/maximum bounds");
      if (rc == H2_PAL_OK && scenario == 2)
        fails += expect(
            !strcmp(page.items[0].tags[0], "Tag: \xe9\x98\x85\xe8\xaf\xbb "),
            "tags remain opaque UTF-8 strings");
    }
  }
  return fails;
}

int h2_gizclaw_home_tests(void) {
  int fails = test_workflow_tags() + test_safety_options();
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
      workflow_page.count == 1u && workflow_page.items[0].tag_count == 1u &&
          strcmp(workflow_page.items[0].tags[0], "assistants") == 0 &&
          strcmp(workflow_page.items[0].name, "story.aesop") == 0 &&
          workflow_page.items[0].i18n_count == 1u &&
          strcmp(workflow_page.items[0].i18n[0].locale, "zh-CN") == 0 &&
          strcmp(workflow_page.items[0].i18n[0].display_name, "全能对话") ==
              0 &&
          strcmp(workflow_page.items[0].i18n[0].description, "default") == 0 &&
          strcmp(workflow_page.runtime_profile_name, "demo") == 0 &&
          strcmp(workflow_page.runtime_profile_revision, "r1") == 0,
      "workflow projection preserves tags name i18n and revision");
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
