#include "h2_gizclaw_e2e_metadata.h"

#include <stdio.h>
#include <string.h>

#define TIMEOUT_MS 30000u
#define MAX_PAGES 32u

static int evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static bool owns(const h2_gizclaw_resp_storage_t *s, const void *p, size_t n) {
  const uintptr_t b = (uintptr_t)s->data, a = (uintptr_t)p;
  return p != NULL && s->used <= s->capacity && a >= b &&
         a - b <= s->used && n <= s->used - (a - b);
}

static bool text(const h2_gizclaw_resp_storage_t *s, const char *p, size_t max) {
  if (!owns(s, p, 1u)) return false;
  const size_t available = s->used - ((uintptr_t)p - (uintptr_t)s->data);
  const char *end = memchr(p, '\0', available);
  return end != NULL && (size_t)(end - p) <= max;
}

static bool revision(const h2_gizclaw_e2e_fixture_t *f,
                     const h2_gizclaw_resp_storage_t *s,
                     const char *profile, const char *rev) {
  return text(s, profile, sizeof(f->runtime_profile_name) - 1u) &&
         strcmp(profile, f->runtime_profile_name) == 0 &&
         text(s, rev, 255u) && rev[0] != '\0';
}

static int config_call(h2_gizclaw_e2e_fixture_t *f,
                       h2_gizclaw_resp_storage_t *s, bool req, bool get,
                       const char *arg, h2_gizclaw_app_config_page_t *page,
                       h2_gizclaw_app_config_value_t *value) {
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT_MS)) return H2_PAL_ERR_TIMEOUT;
  s->used = 0u;
  h2_gizclaw_service_t *service = f->actors[0].service;
  h2_gizclaw_str_t argument = h2_gizclaw_e2e_str(arg);
  if (!req)
    return get ? evidence("h2_gizclaw_rpc_app_config_get", "app-config",
        h2_gizclaw_rpc_app_config_get(service, argument, TIMEOUT_MS, s, value))
      : evidence("h2_gizclaw_rpc_app_config_list", "app-config",
        h2_gizclaw_rpc_app_config_list(service, argument, 16u, TIMEOUT_MS, s, page));
  h2_gizclaw_req_t *r = NULL;
  int rc = get ? evidence("h2_gizclaw_req_create_app_config_get", "app-config",
      h2_gizclaw_req_create_app_config_get(service, 61u, argument, TIMEOUT_MS, &r))
    : evidence("h2_gizclaw_req_create_app_config_list", "app-config",
      h2_gizclaw_req_create_app_config_list(service, 60u, argument, 16u, TIMEOUT_MS, &r));
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_req_do", "app-config",
                   h2_gizclaw_req_do(r, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_req_wait", "app-config", h2_gizclaw_req_wait(r, TIMEOUT_MS));
  if (rc == H2_PAL_OK)
    rc = get ? evidence("h2_gizclaw_resp_parse_app_config_get", "app-config",
        h2_gizclaw_resp_parse_app_config_get(r, s, value))
      : evidence("h2_gizclaw_resp_parse_app_config_list", "app-config",
        h2_gizclaw_resp_parse_app_config_list(r, s, page));
  if (rc != H2_PAL_OK && r) (void)h2_gizclaw_req_cancel(r);
  h2_gizclaw_req_release(r);
  return rc;
}

int h2_gizclaw_e2e_run_app_config(h2_gizclaw_e2e_fixture_t *f,
                                 h2_gizclaw_resp_storage_t *s) {
  if (!f || !s || !s->data || !f->actors[0].service) return H2_PAL_ERR_INVALID_ARG;
  char first_key[H2_GIZCLAW_APP_CONFIG_KEY_MAX_BYTES + 1u] = {0};
  char profile_revision[256] = {0};
  uint8_t first_value[H2_GIZCLAW_APP_CONFIG_VALUE_MAX_BYTES + 1u];
  size_t first_length = 0u;
  int rc = H2_PAL_OK;
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    const bool req = api == 0u;
    char cursor[H2_GIZCLAW_APP_CONFIG_CURSOR_MAX_BYTES + 1u] = {0};
    bool complete = false;
    size_t keys = 0u;
    for (unsigned n = 0u; n < MAX_PAGES && rc == H2_PAL_OK; ++n) {
      h2_gizclaw_app_config_page_t page = {0};
      rc = config_call(f, s, req, false, cursor, &page, NULL);
      if (rc != H2_PAL_OK) break;
      if (!revision(f, s, page.runtime_profile_name, page.runtime_profile_revision) ||
          page.count > 16u || (page.count &&
          (!owns(s, page.keys, page.count * sizeof(*page.keys)) ||
           (uintptr_t)page.keys % _Alignof(char *) != 0u)) ||
          (page.has_next && (!page.count || !text(s, page.next_cursor,
              H2_GIZCLAW_APP_CONFIG_CURSOR_MAX_BYTES) || !page.next_cursor[0] ||
              strcmp(page.next_cursor, cursor) == 0))) {
        rc = H2_PAL_ERR_FORMAT; break;
      }
      if (profile_revision[0] == '\0') strcpy(profile_revision, page.runtime_profile_revision);
      if (strcmp(profile_revision, page.runtime_profile_revision) != 0) {
        rc = H2_PAL_ERR_INVALID_STATE; break;
      }
      for (size_t i = 0u; i < page.count; ++i) {
        if (!text(s, page.keys[i], H2_GIZCLAW_APP_CONFIG_KEY_MAX_BYTES) ||
            !page.keys[i][0]) {
          rc = H2_PAL_ERR_FORMAT; break;
        }
        for (size_t j = 0u; j < i; ++j)
          if (strcmp(page.keys[j], page.keys[i]) == 0) rc = H2_PAL_ERR_FORMAT;
        if (rc != H2_PAL_OK) break;
        if (first_key[0] == '\0') strcpy(first_key, page.keys[i]);
        ++keys;
      }
      if (rc != H2_PAL_OK) break;
      if (!page.has_next) { complete = true; break; }
      strcpy(cursor, page.next_cursor);
    }
    if (rc == H2_PAL_OK && !complete) rc = H2_PAL_ERR_NO_SPACE;
    if (rc == H2_PAL_OK && keys == 0u) rc = H2_PAL_ERR_NOT_FOUND;
    evidence(req ? "h2_gizclaw_resp_parse_app_config_list" : "h2_gizclaw_rpc_app_config_list",
             "app_config_list-assert", rc);
    if (rc != H2_PAL_OK) break;
    h2_gizclaw_app_config_value_t value = {0};
    rc = config_call(f, s, req, true, first_key, NULL, &value);
    if (rc == H2_PAL_OK &&
        (!revision(f, s, value.runtime_profile_name, value.runtime_profile_revision) ||
         strcmp(profile_revision, value.runtime_profile_revision) != 0 ||
         value.value.len > H2_GIZCLAW_APP_CONFIG_VALUE_MAX_BYTES ||
         !owns(s, value.value.data, value.value.len + 1u) ||
         value.value.data[value.value.len] != '\0'))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK && api == 0u) {
      first_length = value.value.len;
      memcpy(first_value, value.value.data, first_length);
    } else if (rc == H2_PAL_OK && (value.value.len != first_length ||
                memcmp(first_value, value.value.data, first_length) != 0))
      rc = H2_PAL_ERR_INVALID_STATE;
    evidence(req ? "h2_gizclaw_resp_parse_app_config_get" : "h2_gizclaw_rpc_app_config_get",
             "app_config_get-assert", rc);
    printf("H2_GIZCLAW_E2E stage=app-config api=%s keys=%zu "
           "value_bytes=%zu result=%s rc=%d\n", req ? "request" : "rpc", keys,
           rc == H2_PAL_OK ? value.value.len : 0u, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  }
  memset(first_value, 0, sizeof(first_value));
  return rc;
}

int h2_gizclaw_e2e_run_public_profile(h2_gizclaw_e2e_fixture_t *f,
                                     h2_gizclaw_resp_storage_t *s) {
  if (!f || !s || !s->data || !f->actors[0].service) return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_profile_t own = {0};
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT_MS)) return H2_PAL_ERR_TIMEOUT;
  int rc = h2_gizclaw_rpc_profile_get(f->actors[0].service, TIMEOUT_MS, &own);
  if (rc != H2_PAL_OK) return rc;
  if (!own.has_name || !own.has_emoji || !memchr(own.name, 0, sizeof(own.name)) ||
      !memchr(own.emoji, 0, sizeof(own.emoji))) return H2_PAL_ERR_FORMAT;
  const h2_gizclaw_str_t keys[] = {h2_gizclaw_e2e_str(f->actors[0].public_key),
                                  h2_gizclaw_e2e_str(f->actors[0].public_key)};
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT_MS)) return H2_PAL_ERR_TIMEOUT;
    h2_gizclaw_public_profile_list_t list = {0};
    s->used = 0u;
    const char *symbol = api == 0u ? "h2_gizclaw_resp_parse_public_profile_get"
                                   : "h2_gizclaw_rpc_public_profile_get";
    if (api == 0u) {
      h2_gizclaw_req_t *r = NULL;
      rc = evidence("h2_gizclaw_req_create_public_profile_get", "public-profile",
          h2_gizclaw_req_create_public_profile_get(f->actors[0].service, 62u,
                                                   keys, 2u, TIMEOUT_MS, &r));
      if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_req_do", "public-profile",
          h2_gizclaw_req_do(r, NULL, NULL, NULL, NULL));
      if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_req_wait", "public-profile",
          h2_gizclaw_req_wait(r, TIMEOUT_MS));
      if (rc == H2_PAL_OK) rc = evidence(symbol, "public-profile",
          h2_gizclaw_resp_parse_public_profile_get(r, s, &list));
      if (rc != H2_PAL_OK && r) (void)h2_gizclaw_req_cancel(r);
      h2_gizclaw_req_release(r);
    } else {
      rc = evidence(symbol, "public-profile", h2_gizclaw_rpc_public_profile_get(
          f->actors[0].service, keys, 2u, TIMEOUT_MS, s, &list));
    }
    if (rc == H2_PAL_OK &&
        (list.count != 1u || !owns(s, list.items, sizeof(*list.items)) ||
         (uintptr_t)list.items % _Alignof(h2_gizclaw_public_profile_t) != 0u ||
         !text(s, list.items[0].peer_public_key, H2_GIZCLAW_PEER_PUBLIC_KEY_MAX_BYTES) ||
         !text(s, list.items[0].display_name, sizeof(own.name) - 1u) ||
         !text(s, list.items[0].emoji, sizeof(own.emoji) - 1u) ||
         strcmp(list.items[0].peer_public_key, f->actors[0].public_key) ||
         strcmp(list.items[0].display_name, own.name) ||
         strcmp(list.items[0].emoji, own.emoji))) rc = H2_PAL_ERR_FORMAT;
    evidence(symbol, "public_profile_get-assert", rc);
  }
  return rc;
}
