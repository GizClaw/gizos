#ifndef H2_GIZCLAW_E2E_INTERNAL_H
#define H2_GIZCLAW_E2E_INTERNAL_H

#include "h2_gizclaw.h"
#include "h2_gizclaw_e2e.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_E2E_ACTOR_COUNT 3u
#define H2_GIZCLAW_E2E_NAME_CAPACITY 256u

typedef enum h2_gizclaw_e2e_actor_role {
  H2_GIZCLAW_E2E_OWNER = 0,
  H2_GIZCLAW_E2E_FRIEND,
  H2_GIZCLAW_E2E_GROUP_MEMBER,
} h2_gizclaw_e2e_actor_role_t;

typedef struct h2_gizclaw_e2e_actor {
  h2_gizclaw_service_t *service;
  h2_gizclaw_config_t config;
  char private_key[H2_PAL_CRYPTO_X25519_KEY_SIZE * 2u + 1u];
  char public_key[H2_PAL_CRYPTO_X25519_KEY_SIZE * 2u + 1u];
  bool registered;
  bool peer_delete_required;
  bool peer_delete_requested;
  bool client_info_requested;
  bool client_identifiers_requested;
} h2_gizclaw_e2e_actor_t;

/* Callback storage belongs to the fixture, not the call/wait stack. A failed
 * wait may leave accepted hooks pending until fixture shutdown drains them. */
typedef struct h2_gizclaw_e2e_speed_hooks {
  h2_gizclaw_req_t *request;
  size_t expected_bytes, bytes, chunks;
  unsigned responses, eos, completions;
  int error, result;
} h2_gizclaw_e2e_speed_hooks_t;

typedef struct h2_gizclaw_e2e_fixture {
  h2_runtime_t *runtime;
  const h2_gizclaw_e2e_config_t *config;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_crypto_api_t *crypto;
  const h2_pal_http_api_t *http;
  const h2_pal_log_api_t *log;
  const h2_pal_time_api_t *time;
  const h2_pal_webrtc_api_t *webrtc;
  h2_gizclaw_e2e_actor_t actors[H2_GIZCLAW_E2E_ACTOR_COUNT];
  uint64_t started_ms;
  uint64_t deadline_ms;
  char endpoint[H2_GIZCLAW_E2E_ENDPOINT_CAPACITY];
  char *registration_token;
  char runtime_profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  char run_prefix[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char workflow_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char workspace_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char pet_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char contact_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char friend_id[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char friend_group_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char friend_group_workspace_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  char friend_group_member_id[H2_GIZCLAW_E2E_NAME_CAPACITY];
  const uint8_t *pcm;
  size_t pcm_len;
  /* Owned by the heap fixture, including after a failed Track unset. */
  h2_gizclaw_track_t *speech_track;
  atomic_size_t speech_offset;
  bool speech_track_bound;
  /* A case may retain borrowed Track/hook state through failed teardown. */
  void *case_state;
  int (*case_cleanup)(struct h2_gizclaw_e2e_fixture *fixture);
  bool workspace_created;
  h2_gizclaw_e2e_actor_role_t workspace_actor_role;
  /* A valid delete reply is retained until cleanup records the handoff. */
  bool workspace_delete_acknowledged;
  bool pet_created;
  bool pet_delete_acknowledged;
  /* Download sinks may outlive a failed wait; keep their state with Service. */
  atomic_size_t pet_download_bytes;
  atomic_size_t group_audio_bytes[2];
  /* Do not reuse sink state after a failed wait while Service may still own it.
   */
  bool group_audio_started;
  h2_gizclaw_e2e_speed_hooks_t speed_hooks[6];
  bool contact_created;
  bool friendship_created;
  bool friend_group_created;
  bool friend_invite_created;
  bool friend_group_invite_created;
  bool friend_group_member_joined;
  bool isolation_workspace_pending;
  bool isolation_workspace_delete_acknowledged;
  bool isolation_contact_pending;
  bool isolation_group_pending;
  bool isolation_pet_pending;
  bool isolation_pet_delete_acknowledged;
  bool cancel_requested;
  bool cleanup_started;
} h2_gizclaw_e2e_fixture_t;

int h2_gizclaw_e2e_fixture_init(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_runtime_t *runtime,
                                const h2_gizclaw_e2e_config_t *config,
                                uint32_t suite_timeout_ms);
int h2_gizclaw_e2e_fixture_connect_actors(h2_gizclaw_e2e_fixture_t *fixture,
                                          size_t actor_count);
int h2_gizclaw_e2e_fixture_reconnect_actor(h2_gizclaw_e2e_fixture_t *fixture,
                                           h2_gizclaw_e2e_actor_role_t role);
int h2_gizclaw_e2e_fixture_poll(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_gizclaw_e2e_actor_role_t role,
                                uint32_t duration_ms);
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t required_ms);
int h2_gizclaw_e2e_fixture_set_deadline(h2_gizclaw_e2e_fixture_t *fixture,
                                        uint32_t timeout_ms);
void h2_gizclaw_e2e_fixture_reset_rpc_channel_observation(void);
int h2_gizclaw_e2e_fixture_rpc_channel_observation(
    size_t *out_max_open, size_t *out_unique_stream_ids,
    size_t *out_open_channels);
int h2_gizclaw_e2e_fixture_cleanup(h2_gizclaw_e2e_fixture_t *fixture);
size_t h2_gizclaw_e2e_fixture_emit_recovery_ledger(
    const h2_gizclaw_e2e_fixture_t *fixture);
/* On failure the caller must retain the fixture and its providers, then retry.
 */
int h2_gizclaw_e2e_fixture_deinit(h2_gizclaw_e2e_fixture_t *fixture);

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value);
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int result);

#ifdef __cplusplus
}
#endif

#endif
