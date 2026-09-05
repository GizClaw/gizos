#include "h2_gizclaw_e2e_rpc.h"
#include "h2_gizclaw_e2e_contact.h"
#include "h2_gizclaw_e2e_friend.h"
#include "h2_gizclaw_e2e_group.h"
#include "h2_gizclaw_e2e_group_audio.h"
#include "h2_gizclaw_e2e_group_message.h"
#include "h2_gizclaw_e2e_pet.h"
#include "h2_gizclaw_e2e_point.h"
#include "h2_gizclaw_e2e_profile.h"
#include "h2_gizclaw_e2e_report.h"
#include "h2_gizclaw_e2e_speech.h"
#include "h2_gizclaw_e2e_telemetry.h"
#include "h2_gizclaw_e2e_voice.h"
#include "h2_gizclaw_e2e_workflow.h"
#include "h2_gizclaw_e2e_workspace.h"
#include "h2_gizclaw_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int append_run_suffix(char *destination, size_t capacity,
                             const char *run_prefix, const char *suffix) {
  if (destination == NULL || capacity == 0u || run_prefix == NULL ||
      suffix == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t prefix_len = strlen(run_prefix);
  const size_t suffix_len = strlen(suffix);
  if (prefix_len >= capacity || suffix_len >= capacity - prefix_len)
    return H2_PAL_ERR_FORMAT;
  memcpy(destination, run_prefix, prefix_len);
  memcpy(destination + prefix_len, suffix, suffix_len + 1u);
  return H2_PAL_OK;
}

bool h2_gizclaw_e2e_workspace_response_ready(
    const h2_gizclaw_workspace_t *workspace, const char *expected_name) {
  return workspace != NULL && workspace->available && workspace->name != NULL &&
         expected_name != NULL && strcmp(workspace->name, expected_name) == 0;
}

static int run_catalog_workspace(h2_gizclaw_e2e_fixture_t *fixture,
                                 h2_gizclaw_resp_storage_t *storage) {
  int rc = h2_gizclaw_e2e_run_workflow(fixture, storage);
  return rc == H2_PAL_OK ? h2_gizclaw_e2e_run_workspace(fixture, storage, true)
                         : rc;
}

static int run_workspace_reconnect(h2_gizclaw_e2e_fixture_t *fixture,
                                   h2_gizclaw_resp_storage_t *storage) {
  int rc =
      h2_gizclaw_e2e_fixture_reconnect_actor(fixture, H2_GIZCLAW_E2E_OWNER);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_workspace_t workspace = {0};
  h2_gizclaw_workspace_get_result_t workspace_result = {0};
  rc = h2_gizclaw_rpc_workspace_get(service,
                                    h2_gizclaw_e2e_str(fixture->workspace_name),
                                    30000u, storage, &workspace_result);
  workspace = workspace_result.workspace;
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  storage->used = 0u;
  h2_gizclaw_workspace_history_page_t history = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_rpc_workspace_history_list(
        service, h2_gizclaw_e2e_str(fixture->workspace_name),
        (h2_gizclaw_str_t){0}, 32u, H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC,
        30000u, storage, &history);
  }
  storage->used = 0u;
  printf("H2_GIZCLAW_E2E stage=workspace_reconnect result=%s\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL");
  return rc;
}

static int run_group(h2_gizclaw_e2e_fixture_t *fixture,
                     h2_gizclaw_resp_storage_t *storage) {
  int rc = h2_gizclaw_e2e_run_group_management(fixture, storage);
  if (rc != H2_PAL_OK)
    return rc;
  char history_id[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = {0};
  rc = h2_gizclaw_e2e_generate_group_message(fixture, history_id,
                                             sizeof(history_id));
  if (rc != H2_PAL_OK)
    return rc;

  rc = h2_gizclaw_e2e_run_group_message(fixture, storage,
                                        h2_gizclaw_e2e_str(history_id));
  if (rc != H2_PAL_OK)
    return rc;
  return h2_gizclaw_e2e_run_group_audio(fixture, storage,
                                        h2_gizclaw_e2e_str(history_id));
}

static int run_gameplay(h2_gizclaw_e2e_fixture_t *fixture,
                        h2_gizclaw_resp_storage_t *storage) {
  int rc = h2_gizclaw_e2e_run_pet(fixture, storage);
  return rc == H2_PAL_OK ? h2_gizclaw_e2e_run_point(fixture, storage) : rc;
}

static void keep_first_failure(int candidate, int *result) {
  if (*result == H2_PAL_OK && candidate != H2_PAL_OK)
    *result = candidate;
}

static int run_peer_name_isolation(h2_gizclaw_e2e_fixture_t *fixture,
                                   h2_gizclaw_resp_storage_t *storage) {
  h2_gizclaw_service_t *service =
      fixture->actors[H2_GIZCLAW_E2E_FRIEND].service;
  if (service == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  char peer_display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  int rc = append_run_suffix(peer_display_name, sizeof(peer_display_name),
                             fixture->run_prefix, " peer-b");
  if (rc != H2_PAL_OK)
    return rc;
  int result = H2_PAL_OK;

  h2_gizclaw_workspace_t workspace = {0};
  h2_gizclaw_workspace_get_result_t workspace_result = {0};
  fixture->isolation_workspace_pending = true;
  fixture->isolation_workspace_delete_acknowledged = false;
  rc = h2_gizclaw_rpc_workspace_create(
      service, h2_gizclaw_e2e_str("assistants"),
      h2_gizclaw_e2e_str(fixture->workflow_name),
      h2_gizclaw_e2e_str(fixture->workspace_name), 30000u, storage, &workspace);
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  storage->used = 0u;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_rpc_workspace_get(
        service, h2_gizclaw_e2e_str(fixture->workspace_name), 30000u, storage,
        &workspace_result);
    workspace = workspace_result.workspace;
    if (rc == H2_PAL_OK &&
        (workspace.name == NULL ||
         strcmp(workspace.name, fixture->workspace_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    storage->used = 0u;
  }
  keep_first_failure(rc, &result);

  h2_gizclaw_contact_t contact = {0};
  if (result == H2_PAL_OK) {
    fixture->isolation_contact_pending = true;
    rc = h2_gizclaw_rpc_contact_create(
        service, h2_gizclaw_e2e_str(fixture->contact_name),
        h2_gizclaw_e2e_str(peer_display_name),
        h2_gizclaw_e2e_str("+8613900000690"), 30000u, storage, &contact);
    if (rc == H2_PAL_OK &&
        (contact.name == NULL || contact.display_name == NULL ||
         strcmp(contact.name, fixture->contact_name) != 0 ||
         strcmp(contact.display_name, peer_display_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    storage->used = 0u;
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_rpc_contact_get(service,
                                      h2_gizclaw_e2e_str(fixture->contact_name),
                                      30000u, storage, &contact);
      if (rc == H2_PAL_OK &&
          (contact.name == NULL || contact.display_name == NULL ||
           strcmp(contact.name, fixture->contact_name) != 0 ||
           strcmp(contact.display_name, peer_display_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      storage->used = 0u;
    }
    keep_first_failure(rc, &result);
  }

  h2_gizclaw_friend_group_t group = {0};
  if (result == H2_PAL_OK) {
    fixture->isolation_group_pending = true;
    rc = h2_gizclaw_rpc_friend_group_create(
        service, h2_gizclaw_e2e_str(fixture->friend_group_name),
        h2_gizclaw_e2e_str(peer_display_name), (h2_gizclaw_str_t){0}, 30000u,
        storage, &group);
    if (rc == H2_PAL_OK &&
        (group.name == NULL || group.display_name == NULL ||
         strcmp(group.name, fixture->friend_group_name) != 0 ||
         strcmp(group.display_name, peer_display_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    storage->used = 0u;
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_rpc_friend_group_get(
          service, h2_gizclaw_e2e_str(fixture->friend_group_name), 30000u,
          storage, &group);
      if (rc == H2_PAL_OK &&
          (group.name == NULL || group.display_name == NULL ||
           strcmp(group.name, fixture->friend_group_name) != 0 ||
           strcmp(group.display_name, peer_display_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      storage->used = 0u;
    }
    keep_first_failure(rc, &result);
  }

  h2_gizclaw_pet_t pet = {0};
  if (result == H2_PAL_OK) {
    const h2_gizclaw_pet_adopt_options_t options = {
        .name = h2_gizclaw_e2e_str(fixture->pet_name),
        .display_name = h2_gizclaw_e2e_str(peer_display_name),
    };
    fixture->isolation_pet_pending = true;
    fixture->isolation_pet_delete_acknowledged = false;
    rc = h2_gizclaw_rpc_pet_adopt(service, &options, 30000u, storage, &pet);
    if (rc == H2_PAL_OK &&
        (pet.name == NULL || strcmp(pet.name, fixture->pet_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    storage->used = 0u;
    if (rc == H2_PAL_OK) {
      rc =
          h2_gizclaw_rpc_pet_get(service, h2_gizclaw_e2e_str(fixture->pet_name),
                                 30000u, storage, &pet);
      if (rc == H2_PAL_OK &&
          (pet.name == NULL || strcmp(pet.name, fixture->pet_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      storage->used = 0u;
    }
    keep_first_failure(rc, &result);
  }

  /* Fixture cleanup owns all four obligations, also on timeout or failure. */
  printf("H2_GIZCLAW_E2E stage=peer_name_isolation result=%s\n",
         result == H2_PAL_OK ? "PASS" : "FAIL");
  return result;
}

int h2_gizclaw_e2e_run_rpc(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  enum rpc_domain_index {
    RPC_DOMAIN_PROFILE = 0,
    RPC_DOMAIN_CATALOG_WORKSPACE,
    RPC_DOMAIN_SPEECH,
    RPC_DOMAIN_WORKSPACE_RECONNECT,
    RPC_DOMAIN_CONTACT,
    RPC_DOMAIN_FRIEND,
    RPC_DOMAIN_GROUP,
    RPC_DOMAIN_GAMEPLAY,
    RPC_DOMAIN_PEER_NAME_ISOLATION,
    RPC_DOMAIN_TELEMETRY,
    RPC_DOMAIN_COUNT,
  };
  struct rpc_domain {
    const char *name;
    int (*run)(h2_gizclaw_e2e_fixture_t *fixture,
               h2_gizclaw_resp_storage_t *storage);
    uint16_t dependencies;
  };
  static const struct rpc_domain domains[] = {
      [RPC_DOMAIN_PROFILE] = {"profile", h2_gizclaw_e2e_run_profile, 0u},
      [RPC_DOMAIN_CATALOG_WORKSPACE] = {"catalog-workspace",
                                        run_catalog_workspace, 0u},
      [RPC_DOMAIN_SPEECH] = {"speech", h2_gizclaw_e2e_run_speech, 0u},
      [RPC_DOMAIN_WORKSPACE_RECONNECT] = {"workspace-reconnect",
                                          run_workspace_reconnect,
                                          1u << RPC_DOMAIN_CATALOG_WORKSPACE},
      [RPC_DOMAIN_CONTACT] = {"contact", h2_gizclaw_e2e_run_contact, 0u},
      [RPC_DOMAIN_FRIEND] = {"friend", h2_gizclaw_e2e_run_friend, 0u},
      [RPC_DOMAIN_GROUP] = {"group", run_group, 0u},
      [RPC_DOMAIN_GAMEPLAY] = {"gameplay", run_gameplay, 0u},
      [RPC_DOMAIN_PEER_NAME_ISOLATION] = {"peer-name-isolation",
                                          run_peer_name_isolation,
                                          (1u << RPC_DOMAIN_CATALOG_WORKSPACE) |
                                              (1u << RPC_DOMAIN_CONTACT) |
                                              (1u << RPC_DOMAIN_GROUP) |
                                              (1u << RPC_DOMAIN_GAMEPLAY)},
      [RPC_DOMAIN_TELEMETRY] = {"telemetry", h2_gizclaw_e2e_run_telemetry, 0u},
  };
  _Static_assert(sizeof(domains) / sizeof(domains[0]) == RPC_DOMAIN_COUNT,
                 "RPC domain table and index must remain synchronized");
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  for (size_t index = 0u; index < RPC_DOMAIN_COUNT; ++index) {
    if (h2_gizclaw_e2e_report_select(&report, domains[index].name) !=
        H2_PAL_OK) {
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  void *scratch = h2_pal_mem_alloc(fixture->allocator, 65536u);
  if (scratch == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  h2_gizclaw_resp_storage_t storage = {.data = scratch, .capacity = 65536u};
  int first_rc = H2_PAL_OK;
  for (size_t index = 0u; index < RPC_DOMAIN_COUNT; ++index) {
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/%s\n",
           domains[index].name);
    h2_gizclaw_e2e_case_status_t status = H2_GIZCLAW_E2E_CASE_PASS;
    const char *blocked_by = NULL;
    int domain_rc = H2_PAL_OK;
    const bool cancelled =
        fixture->cancel_requested ||
        (fixture->config->should_stop != NULL &&
         fixture->config->should_stop(fixture->config->should_stop_user));
    if (cancelled) {
      status = H2_GIZCLAW_E2E_CASE_CANCELLED;
      domain_rc = H2_PAL_ERR_CLOSED;
    } else {
      for (size_t dependency = 0u; dependency < index; ++dependency) {
        if ((domains[index].dependencies & (1u << dependency)) != 0u &&
            report.cases[dependency].status != H2_GIZCLAW_E2E_CASE_PASS) {
          status = H2_GIZCLAW_E2E_CASE_BLOCKED;
          blocked_by = domains[dependency].name;
          domain_rc = report.cases[dependency].rc;
          break;
        }
      }
      if (status == H2_GIZCLAW_E2E_CASE_PASS) {
        storage.used = 0u;
        domain_rc = domains[index].run(fixture, &storage);
        if (domain_rc != H2_PAL_OK) {
          status = H2_GIZCLAW_E2E_CASE_FAIL;
        }
      }
    }
    const int report_rc = h2_gizclaw_e2e_report_terminal(
        &report, domains[index].name, status, domain_rc, blocked_by);
    if (report_rc != H2_PAL_OK) {
      h2_pal_mem_free(fixture->allocator, scratch);
      return report_rc;
    }
    printf("H2_GIZCLAW_E2E stage=rpc-domain case=%s status=%s rc=%d "
           "blocked_by=%s\n",
           domains[index].name, h2_gizclaw_e2e_case_status_name(status),
           domain_rc, blocked_by == NULL ? "-" : blocked_by);
    printf("H2_GIZCLAW_E2E stage=coverage-end case=rpc/%s status=%s rc=%d "
           "cleanup_rc=0\n",
           domains[index].name, h2_gizclaw_e2e_case_status_name(status),
           domain_rc);
    if (first_rc == H2_PAL_OK && status != H2_GIZCLAW_E2E_CASE_PASS) {
      first_rc = domain_rc;
    }
  }
  h2_gizclaw_e2e_report_cleanup(&report, H2_PAL_OK);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&report);
  printf("H2_GIZCLAW_E2E stage=rpc-summary selected=%zu pass=%zu fail=%zu "
         "error=%zu blocked=%zu cancelled=%zu terminal=%zu complete=%s\n",
         summary.selected, summary.pass, summary.fail, summary.error,
         summary.blocked, summary.cancelled, summary.terminal,
         summary.complete ? "true" : "false");
  h2_pal_mem_free(fixture->allocator, scratch);
  if (!summary.complete) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return first_rc;
}

int h2_gizclaw_e2e_prepare_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  void *scratch = h2_pal_mem_alloc(fixture->allocator, 65536u);
  if (scratch == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  h2_gizclaw_resp_storage_t storage = {.data = scratch, .capacity = 65536u};
  int rc = h2_gizclaw_e2e_run_workflow(fixture, &storage);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_run_workspace(fixture, &storage, false);
  h2_pal_mem_free(fixture->allocator, scratch);
  return rc;
}
