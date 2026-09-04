#include "h2_gizclaw_e2e_profile.h"

#include <string.h>

#define PROFILE_TIMEOUT_MS 30000u

enum profile_method { PROFILE_GET, PROFILE_NAME, PROFILE_EMOJI };

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static int validate(const char *symbol, const h2_gizclaw_profile_t *profile,
                    const char *name, const char *emoji) {
  const bool matches =
      (name == NULL ||
       (profile->has_name &&
        strncmp(profile->name, name, sizeof(profile->name)) == 0)) &&
      (emoji == NULL ||
       (profile->has_emoji &&
        strncmp(profile->emoji, emoji, sizeof(profile->emoji)) == 0));
  return checked(symbol, "profile-assert",
                 matches ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE);
}

static int request_profile(h2_gizclaw_e2e_fixture_t *fixture,
                           enum profile_method method, uint64_t identity,
                           h2_gizclaw_str_t value,
                           h2_gizclaw_profile_t *profile) {
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, PROFILE_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_req_t *request = NULL;
  int rc;
  switch (method) {
  case PROFILE_GET:
    rc = checked("h2_gizclaw_req_create_profile_get", "profile-req",
                 h2_gizclaw_req_create_profile_get(
                     service, identity, PROFILE_TIMEOUT_MS, &request));
    break;
  case PROFILE_NAME:
    rc = checked("h2_gizclaw_req_create_profile_put_name", "profile-req",
                 h2_gizclaw_req_create_profile_put_name(
                     service, identity, value, PROFILE_TIMEOUT_MS, &request));
    break;
  case PROFILE_EMOJI:
    rc = checked("h2_gizclaw_req_create_profile_put_emoji", "profile-req",
                 h2_gizclaw_req_create_profile_put_emoji(
                     service, identity, value, PROFILE_TIMEOUT_MS, &request));
    break;
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_req_do", "profile-req",
                 h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_req_wait", "profile-req",
                 h2_gizclaw_req_wait(request, PROFILE_TIMEOUT_MS));
  if (rc == H2_PAL_OK) {
    switch (method) {
    case PROFILE_GET:
      rc = checked("h2_gizclaw_resp_parse_profile_get", "profile-req",
                   h2_gizclaw_resp_parse_profile_get(request, profile));
      break;
    case PROFILE_NAME:
      rc = checked("h2_gizclaw_resp_parse_profile_put_name", "profile-req",
                   h2_gizclaw_resp_parse_profile_put_name(request, profile));
      break;
    case PROFILE_EMOJI:
      rc = checked("h2_gizclaw_resp_parse_profile_put_emoji", "profile-req",
                   h2_gizclaw_resp_parse_profile_put_emoji(request, profile));
      break;
    }
  }
  if (rc != H2_PAL_OK && request != NULL)
    (void)checked("h2_gizclaw_req_cancel", "profile-cleanup",
                  h2_gizclaw_req_cancel(request));
  h2_gizclaw_req_release(request);
  return rc;
}

static bool has_time(h2_gizclaw_e2e_fixture_t *fixture) {
  return h2_gizclaw_e2e_fixture_has_time(fixture, PROFILE_TIMEOUT_MS);
}

int h2_gizclaw_e2e_run_profile(h2_gizclaw_e2e_fixture_t *fixture,
                               h2_gizclaw_resp_storage_t *storage) {
  (void)storage;
  if (fixture == NULL || fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  char sync_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char req_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  const char *prefix_end =
      memchr(fixture->run_prefix, '\0', sizeof(fixture->run_prefix));
  if (prefix_end == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t prefix_len = (size_t)(prefix_end - fixture->run_prefix);
  if (prefix_len == 0u || prefix_len + sizeof("-owner-rpc") > sizeof(sync_name))
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(sync_name, fixture->run_prefix, prefix_len);
  memcpy(sync_name + prefix_len, "-owner-rpc", sizeof("-owner-rpc"));
  memcpy(req_name, fixture->run_prefix, prefix_len);
  memcpy(req_name + prefix_len, "-owner-req", sizeof("-owner-req"));
  const char *sync_emoji = "🤖", *req_emoji = "🧪";
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_profile_t profile = {0};
  if (!has_time(fixture))
    return H2_PAL_ERR_TIMEOUT;
  int rc = checked(
      "h2_gizclaw_rpc_profile_put_name", "profile-rpc",
      h2_gizclaw_rpc_profile_put_name(service, h2_gizclaw_e2e_str(sync_name),
                                      PROFILE_TIMEOUT_MS, &profile));
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_rpc_profile_put_name", &profile, sync_name, NULL);
  if (rc != H2_PAL_OK || !has_time(fixture))
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_TIMEOUT;
  rc = checked("h2_gizclaw_rpc_profile_put_emoji", "profile-rpc",
               h2_gizclaw_rpc_profile_put_emoji(service,
                                                h2_gizclaw_e2e_str(sync_emoji),
                                                PROFILE_TIMEOUT_MS, &profile));
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_rpc_profile_put_emoji", &profile, NULL,
                  sync_emoji);
  if (rc != H2_PAL_OK || !has_time(fixture))
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_TIMEOUT;
  rc = checked(
      "h2_gizclaw_rpc_profile_get", "profile-rpc",
      h2_gizclaw_rpc_profile_get(service, PROFILE_TIMEOUT_MS, &profile));
  if (rc == H2_PAL_OK)
    rc =
        validate("h2_gizclaw_rpc_profile_get", &profile, sync_name, sync_emoji);
  if (rc == H2_PAL_OK)
    rc = request_profile(fixture, PROFILE_GET, 11u, (h2_gizclaw_str_t){0},
                         &profile);
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_resp_parse_profile_get", &profile, sync_name,
                  sync_emoji);
  if (rc == H2_PAL_OK)
    rc = request_profile(fixture, PROFILE_NAME, 12u,
                         h2_gizclaw_e2e_str(req_name), &profile);
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_resp_parse_profile_put_name", &profile, req_name,
                  NULL);
  if (rc == H2_PAL_OK)
    rc = request_profile(fixture, PROFILE_EMOJI, 13u,
                         h2_gizclaw_e2e_str(req_emoji), &profile);
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_resp_parse_profile_put_emoji", &profile, NULL,
                  req_emoji);
  if (rc == H2_PAL_OK)
    rc = request_profile(fixture, PROFILE_GET, 14u, (h2_gizclaw_str_t){0},
                         &profile);
  if (rc == H2_PAL_OK)
    rc = validate("h2_gizclaw_resp_parse_profile_get", &profile, req_name,
                  req_emoji);
  return rc;
}
