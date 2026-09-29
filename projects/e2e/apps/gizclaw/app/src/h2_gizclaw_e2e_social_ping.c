#include "h2_gizclaw_e2e_social_ping.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int h2_gizclaw_e2e_check_social_ping(h2_gizclaw_e2e_fixture_t *f,
                                    bool group, bool req) {
  if (!f || !f->actors[0].service) return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_e2e_fixture_has_time(f, 30000u)) return H2_PAL_ERR_TIMEOUT;
  const h2_gizclaw_e2e_actor_role_t receiver = group ? H2_GIZCLAW_E2E_GROUP_MEMBER : H2_GIZCLAW_E2E_FRIEND;
  h2_gizclaw_e2e_social_observation_t before = {0}, after = {0};
  int rc = h2_gizclaw_e2e_fixture_social_observation(f, receiver, &before);
  if (rc != H2_PAL_OK || before.invalid) return rc ? rc : H2_PAL_ERR_INVALID_STATE;
  const char *method = group ? "friend_group_ping" : "friend_ping";
  char symbol[96], stage[64];
  h2_gizclaw_str_t target = h2_gizclaw_e2e_str(group ? f->friend_group_name : f->friend_id);
  h2_gizclaw_service_t *s = f->actors[0].service;
  h2_gizclaw_social_ping_t result = {0};
  if (req) {
    h2_gizclaw_req_t *request = NULL;
    rc = group ? h2_gizclaw_req_create_friend_group_ping(s, 80u, target, 30000u, &request)
               : h2_gizclaw_req_create_friend_ping(s, 81u, target, 30000u, &request);
    snprintf(symbol, sizeof(symbol), "h2_gizclaw_req_create_%s", method);
    h2_gizclaw_e2e_evidence(symbol, "social-ping", rc);
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
      h2_gizclaw_e2e_evidence("h2_gizclaw_req_do", "social-ping", rc);
    }
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_req_wait(request, 30000u);
      h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait", "social-ping", rc);
    }
    snprintf(symbol, sizeof(symbol), "h2_gizclaw_resp_parse_%s", method);
    if (rc == H2_PAL_OK) {
      rc = group ? h2_gizclaw_resp_parse_friend_group_ping(request, &result)
                 : h2_gizclaw_resp_parse_friend_ping(request, &result);
      h2_gizclaw_e2e_evidence(symbol, "social-ping", rc);
    }
    if (rc != H2_PAL_OK && request) (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  } else {
    rc = group ? h2_gizclaw_rpc_friend_group_ping(s, target, 30000u, &result)
               : h2_gizclaw_rpc_friend_ping(s, target, 30000u, &result);
    snprintf(symbol, sizeof(symbol), "h2_gizclaw_rpc_%s", method);
    h2_gizclaw_e2e_evidence(symbol, "social-ping", rc);
  }
  if (rc == H2_PAL_OK) rc = h2_gizclaw_e2e_fixture_social_observation(f, receiver, &after);
  if (rc == H2_PAL_OK && after.invalid) rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && result.result == H2_GIZCLAW_SOCIAL_PING_RESULT_DELIVERED) {
    if (result.delivered_count != 1u || result.has_retry_after_seconds ||
        after.count != before.count + 1u || strcmp(after.sender, f->actors[0].public_key) ||
        (group ? strcmp(after.group, f->friend_group_name) != 0 : after.group[0] != '\0')) rc = H2_PAL_ERR_INVALID_STATE;
  } else if (rc == H2_PAL_OK && !group && !req &&
             result.result == H2_GIZCLAW_SOCIAL_PING_RESULT_RATE_LIMITED) {
    /* The same peer pair is reused by the two Friend API cycles. A server
     * cooldown must neither claim delivery nor invoke the receiver again. */
    if (before.count == 0u || result.delivered_count != 0u ||
        !result.has_retry_after_seconds || result.retry_after_seconds == 0u ||
        after.count != before.count) rc = H2_PAL_ERR_INVALID_STATE;
  } else if (rc == H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
  snprintf(stage, sizeof(stage), "%s-assert", method);
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  printf("H2_GIZCLAW_E2E stage=social-ping kind=%s api=%s outcome=%u "
         "delivered=%" PRIu32 " received=%" PRIu32 " result=%s rc=%d\n", group ? "group" : "friend",
         req ? "request" : "rpc", (unsigned)result.result, result.delivered_count,
         after.count - before.count, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  return rc;
}
