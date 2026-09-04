#include "h2_gizclaw_e2e_group_audio.h"

#include <string.h>

#define DOWNLOAD_TIMEOUT 30000u
#define METHOD "friend_group_message_audio_download"

static int record(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static int count_audio(void *user, const uint8_t *data, size_t len) {
  if (!user || !data || !len)
    return H2_PAL_ERR_INVALID_ARG;
  atomic_size_t *counter = user;
  size_t count = atomic_load(counter);
  do {
    if (len > SIZE_MAX - count)
      return H2_PAL_ERR_NO_SPACE;
  } while (!atomic_compare_exchange_weak(counter, &count, count + len));
  return H2_PAL_OK;
}
static h2_pal_result_t count_audio_output(void *user, const uint8_t *data,
                                          size_t len, size_t *out_written) {
  h2_pal_result_t rc = (h2_pal_result_t)count_audio(user, data, len);
  *out_written = rc == H2_PAL_OK ? len : 0u;
  return rc;
}

static bool owned_text(const h2_gizclaw_resp_storage_t *storage,
                       const char *text) {
  uintptr_t base = (uintptr_t)storage->data, ptr = (uintptr_t)text;
  return text && storage->used <= storage->capacity && ptr >= base &&
         ptr - base < storage->used && text[0] &&
         memchr(text, '\0', storage->used - (ptr - base));
}

/* data-down output callbacks run only from the application poll context. */
static int wait_download(h2_gizclaw_e2e_fixture_t *fixture,
                         h2_gizclaw_service_t *service,
                         h2_gizclaw_req_t *request) {
  int rc = H2_PAL_OK;
  for (uint32_t elapsed = 0u; rc == H2_PAL_OK && elapsed < DOWNLOAD_TIMEOUT;
       ++elapsed) {
    size_t dispatched = 0u;
    rc = h2_gizclaw_service_poll(service, 8u, &dispatched);
    if (rc != H2_PAL_OK)
      break;
    rc = h2_gizclaw_req_wait(request, 1u);
    if (rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    rc = h2_pal_time_sleep_ms(fixture->time, 1u);
  }
  return rc == H2_PAL_OK ? H2_PAL_ERR_TIMEOUT : rc;
}

int h2_gizclaw_e2e_run_group_audio(h2_gizclaw_e2e_fixture_t *fixture,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_str_t history_id) {
  if (!fixture || !storage || !storage->data || !storage->capacity ||
      !fixture->actors[H2_GIZCLAW_E2E_OWNER].service || !history_id.data ||
      !history_id.len || history_id.len >= H2_GIZCLAW_E2E_NAME_CAPACITY ||
      memchr(history_id.data, '\0', history_id.len))
    return H2_PAL_ERR_INVALID_ARG;
  if (fixture->group_audio_started || !fixture->friend_group_name[0] ||
      !memchr(fixture->friend_group_name, '\0',
              sizeof(fixture->friend_group_name)))
    return H2_PAL_ERR_INVALID_STATE;
  fixture->group_audio_started = true;
  for (unsigned api = 0u; api < 2u; ++api)
    atomic_init(&fixture->group_audio_bytes[api], 0u);
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_str_t group = h2_gizclaw_e2e_str(fixture->friend_group_name);
  int rc = H2_PAL_OK;
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    storage->used = 0u;
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, DOWNLOAD_TIMEOUT)) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    h2_gizclaw_friend_group_message_audio_info_t info = {0};
    const char *symbol =
        api ? "h2_gizclaw_rpc_" METHOD : "h2_gizclaw_resp_parse_" METHOD;
    if (api) {
      rc = record(symbol, "group-audio-rpc",
                  h2_gizclaw_rpc_friend_group_message_audio_download(
                      service, group, history_id, count_audio,
                      &fixture->group_audio_bytes[api], DOWNLOAD_TIMEOUT,
                      storage, &info));
    } else {
      h2_gizclaw_req_t *request = NULL;
      rc = record("h2_gizclaw_req_create_" METHOD, "group-audio-req",
                  h2_gizclaw_req_create_friend_group_message_audio_download(
                      service, 401u, group, history_id, DOWNLOAD_TIMEOUT,
                      &request));
      if (rc == H2_PAL_OK)
        rc = record("h2_gizclaw_req_do", "group-audio-req",
                    h2_gizclaw_req_do(
                        request, &fixture->group_audio_bytes[api], NULL,
                        count_audio_output, NULL));
      if (rc == H2_PAL_OK)
        rc = record("h2_gizclaw_req_wait", "group-audio-req",
                    wait_download(fixture, service, request));
      if (rc == H2_PAL_OK)
        rc = record(symbol, "group-audio-req",
                    h2_gizclaw_resp_parse_friend_group_message_audio_download(
                        request, storage, &info));
      if (request) {
        if (rc != H2_PAL_OK)
          (void)record("h2_gizclaw_req_cancel", "group-audio-cleanup",
                       h2_gizclaw_req_cancel(request));
        h2_gizclaw_req_release(request);
      }
    }
    if (rc == H2_PAL_OK) {
      size_t received = atomic_load(&fixture->group_audio_bytes[api]);
      if (!owned_text(storage, info.friend_group_name) ||
          !owned_text(storage, info.history_id) ||
          !owned_text(storage, info.mime_type) ||
          strcmp(info.friend_group_name, fixture->friend_group_name) ||
          strlen(info.history_id) != history_id.len ||
          memcmp(info.history_id, history_id.data, history_id.len) ||
          !received || info.received_bytes != received ||
          info.size_bytes != info.received_bytes)
        rc = H2_PAL_ERR_FORMAT;
      record(symbol, METHOD "-assert", rc);
    }
  }
  storage->used = 0u;
  return rc;
}
