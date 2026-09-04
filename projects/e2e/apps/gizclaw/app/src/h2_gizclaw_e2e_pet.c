#include "h2_gizclaw_e2e_pet.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PET_TIMEOUT 30000u
#define PET_LIMIT 32u
#define PET_PAGES 32u
#define PET_TEXT_MAX 255u

enum method { ADOPT, GET, LIST, DRIVE, ACTIONS, PIXA, DELETE };
union response {
  h2_gizclaw_pet_t pet;
  h2_gizclaw_pet_page_t page;
  h2_gizclaw_pet_actions_t actions;
  h2_gizclaw_pet_pixa_info_t pixa;
};
static const char *const names[] = {
    "pet_adopt",      "pet_get",           "pet_list",  "pet_drive",
    "pet_action_get", "pet_pixa_download", "pet_delete"};
static int record(const char *prefix, enum method method, const char *stage,
                  int rc) {
  char symbol[96];
  (void)snprintf(symbol, sizeof(symbol), "h2_gizclaw_%s_%s", prefix,
                 names[method]);
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
static int proof(bool req, enum method method, int rc) {
  char stage[64];
  (void)snprintf(stage, sizeof(stage), "%s-assert", names[method]);
  return record(req ? "resp_parse" : "rpc", method, stage, rc);
}
static bool owns(const h2_gizclaw_resp_storage_t *s, const void *p, size_t n) {
  uintptr_t base = (uintptr_t)s->data, value = (uintptr_t)p;
  return p && s->used <= s->capacity && value >= base &&
         value - base <= s->used && n <= s->used - (value - base);
}
static bool text(const h2_gizclaw_resp_storage_t *s, const char *p,
                 bool required) {
  if (!p)
    return !required;
  if (!owns(s, p, 1u))
    return false;
  size_t available = s->used - ((uintptr_t)p - (uintptr_t)s->data);
  return (!required || p[0]) && memchr(p, '\0', available) != NULL;
}
static bool pet_valid(const h2_gizclaw_resp_storage_t *s,
                      const h2_gizclaw_pet_t *pet) {
  /* Lifecycle and stats are server-owned. Do not impose a closed enum or
   * assume a profile-specific initial level, reward, or stat range. */
  return text(s, pet->name, true) && text(s, pet->pet_def_name, true) &&
         text(s, pet->display_name, false) &&
         text(s, pet->workspace_name, false) && text(s, pet->died_at, false) &&
         text(s, pet->state_settled_at, false) &&
         text(s, pet->updated_at, false) && isfinite(pet->stats.life) &&
         isfinite(pet->stats.health) && isfinite(pet->stats.satiety) &&
         isfinite(pet->stats.hygiene) && isfinite(pet->stats.mood) &&
         isfinite(pet->stats.energy);
}
static bool matches(h2_gizclaw_e2e_fixture_t *f,
                    const h2_gizclaw_resp_storage_t *s,
                    const h2_gizclaw_pet_t *pet, const char *definition) {
  return pet_valid(s, pet) && strcmp(pet->name, f->pet_name) == 0 &&
         text(s, pet->display_name, true) &&
         strcmp(pet->display_name, "H2 E2E Pet") == 0 &&
         (!definition[0] || strcmp(pet->pet_def_name, definition) == 0);
}
static int count_bytes(void *user, const uint8_t *data, size_t n) {
  h2_gizclaw_e2e_fixture_t *f = user;
  if (!f || !data || !n)
    return H2_PAL_ERR_INVALID_ARG;
  size_t count = atomic_load(&f->pet_download_bytes);
  if (n > SIZE_MAX - count)
    return H2_PAL_ERR_NO_SPACE;
  atomic_store(&f->pet_download_bytes, count + n);
  return H2_PAL_OK;
}
static h2_pal_result_t count_bytes_output(void *user, const uint8_t *data,
                                          size_t n, size_t *out_written) {
  h2_pal_result_t rc = (h2_pal_result_t)count_bytes(user, data, n);
  *out_written = rc == H2_PAL_OK ? n : 0u;
  return rc;
}
static int wait_pixa(h2_gizclaw_e2e_fixture_t *fixture,
                      h2_gizclaw_service_t *service,
                      h2_gizclaw_req_t *request) {
  int rc = H2_PAL_OK;
  for (uint32_t elapsed = 0u; rc == H2_PAL_OK && elapsed < PET_TIMEOUT;
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
static int call(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                bool req, enum method method, uint64_t *identity,
                const char *cursor, const char *key, union response *out) {
  s->used = 0u;
  memset(out, 0, sizeof(*out));
  if (!h2_gizclaw_e2e_fixture_has_time(f, PET_TIMEOUT))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = f->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_str_t name = h2_gizclaw_e2e_str(f->pet_name);
  h2_gizclaw_pet_adopt_options_t adopt = {
      .name = name, .display_name = h2_gizclaw_e2e_str("H2 E2E Pet")};
  h2_gizclaw_pet_drive_options_t drive = {
      .pet_name = name,
      .behavior = H2_GIZCLAW_PET_BEHAVIOR_NONE,
      .idempotency_key = h2_gizclaw_e2e_str(key)};
  if (method == PIXA)
    atomic_store(&f->pet_download_bytes, 0u);
  int rc = H2_PAL_ERR_INVALID_ARG;
  if (!req) {
    if (method == ADOPT) {
      f->pet_created = true;
      f->pet_delete_acknowledged = false;
    }
    switch (method) {
    case ADOPT:
      rc = h2_gizclaw_rpc_pet_adopt(service, &adopt, PET_TIMEOUT, s, &out->pet);
      break;
    case GET:
      rc = h2_gizclaw_rpc_pet_get(service, name, PET_TIMEOUT, s, &out->pet);
      break;
    case DELETE:
      rc = h2_gizclaw_rpc_pet_delete(service, name, PET_TIMEOUT, s, &out->pet);
      break;
    case LIST:
      rc = h2_gizclaw_rpc_pet_list(service, h2_gizclaw_e2e_str(cursor),
                                   PET_LIMIT, PET_TIMEOUT, s, &out->page);
      break;
    case DRIVE:
      rc = h2_gizclaw_rpc_pet_drive(service, &drive, PET_TIMEOUT, s, &out->pet);
      break;
    case ACTIONS:
      rc = h2_gizclaw_rpc_pet_action_get(service, name, PET_TIMEOUT, s,
                                         &out->actions);
      break;
    case PIXA:
      rc = h2_gizclaw_rpc_pet_pixa_download(service, name, count_bytes, f,
                                            PET_TIMEOUT, s, &out->pixa);
      break;
    }
    record("rpc", method, "pet-rpc", rc);
  } else {
    h2_gizclaw_req_t *request = NULL;
    uint64_t id = (*identity)++;
    switch (method) {
    case ADOPT:
      rc = h2_gizclaw_req_create_pet_adopt(service, id, &adopt, PET_TIMEOUT,
                                           &request);
      break;
    case GET:
      rc = h2_gizclaw_req_create_pet_get(service, id, name, PET_TIMEOUT,
                                         &request);
      break;
    case DELETE:
      rc = h2_gizclaw_req_create_pet_delete(service, id, name, PET_TIMEOUT,
                                            &request);
      break;
    case LIST:
      rc = h2_gizclaw_req_create_pet_list(service, id,
                                          h2_gizclaw_e2e_str(cursor), PET_LIMIT,
                                          PET_TIMEOUT, &request);
      break;
    case DRIVE:
      rc = h2_gizclaw_req_create_pet_drive(service, id, &drive, PET_TIMEOUT,
                                           &request);
      break;
    case ACTIONS:
      rc = h2_gizclaw_req_create_pet_action_get(service, id, name, PET_TIMEOUT,
                                                &request);
      break;
    case PIXA:
      rc = h2_gizclaw_req_create_pet_pixa_download(
          service, id, name, PET_TIMEOUT, &request);
      break;
    }
    record("req_create", method, "pet-req", rc);
    if (rc == H2_PAL_OK) {
      if (method == ADOPT) {
        f->pet_created = true;
        f->pet_delete_acknowledged = false;
      }
      rc = h2_gizclaw_req_do(request, method == PIXA ? f : NULL, NULL,
                             method == PIXA ? count_bytes_output : NULL, NULL);
      h2_gizclaw_e2e_evidence("h2_gizclaw_req_do", "pet-req", rc);
    }
    if (rc == H2_PAL_OK) {
      rc = method == PIXA ? wait_pixa(f, service, request)
                          : h2_gizclaw_req_wait(request, PET_TIMEOUT);
      h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait", "pet-req", rc);
    }
    if (rc == H2_PAL_OK) {
      switch (method) {
      case ADOPT:
        rc = h2_gizclaw_resp_parse_pet_adopt(request, s, &out->pet);
        break;
      case GET:
        rc = h2_gizclaw_resp_parse_pet_get(request, s, &out->pet);
        break;
      case DELETE:
        rc = h2_gizclaw_resp_parse_pet_delete(request, s, &out->pet);
        break;
      case LIST:
        rc = h2_gizclaw_resp_parse_pet_list(request, s, &out->page);
        break;
      case DRIVE:
        rc = h2_gizclaw_resp_parse_pet_drive(request, s, &out->pet);
        break;
      case ACTIONS:
        rc = h2_gizclaw_resp_parse_pet_action_get(request, s, &out->actions);
        break;
      case PIXA:
        rc = h2_gizclaw_resp_parse_pet_pixa_download(request, s, &out->pixa);
        break;
      }
      record("resp_parse", method, "pet-req", rc);
    }
    if (request) {
      if (rc != H2_PAL_OK)
        h2_gizclaw_e2e_evidence("h2_gizclaw_req_cancel", "pet-cleanup",
                                h2_gizclaw_req_cancel(request));
      h2_gizclaw_req_release(request);
    }
  }
  return rc == H2_PAL_OK && s->used > s->capacity ? H2_PAL_ERR_FORMAT : rc;
}
static int get(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
               bool req, uint64_t *id, const char *definition) {
  union response r;
  int rc = call(f, s, req, GET, id, "", "", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.pet, definition))
    rc = H2_PAL_ERR_FORMAT;
  return proof(req, GET, rc);
}
static int list(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                bool req, uint64_t *id, const char *definition) {
  char cursor[PET_TEXT_MAX + 1u] = {0};
  size_t found = 0u;
  int rc = H2_PAL_ERR_NO_SPACE;
  for (unsigned page = 0u; page < PET_PAGES; ++page) {
    union response r;
    rc = call(f, s, req, LIST, id, cursor, "", &r);
    if (rc != H2_PAL_OK)
      break;
    h2_gizclaw_pet_page_t *p = &r.page;
    if (p->count > PET_LIMIT ||
        (p->count && (!owns(s, p->items, p->count * sizeof(*p->items)) ||
                      (uintptr_t)p->items % _Alignof(h2_gizclaw_pet_t))) ||
        !text(s, p->next_cursor, p->has_next)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    for (size_t i = 0u; i < p->count && rc == H2_PAL_OK; ++i) {
      if (!pet_valid(s, &p->items[i])) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      for (size_t j = 0u; j < i; ++j)
        if (strcmp(p->items[i].name, p->items[j].name) == 0)
          rc = H2_PAL_ERR_FORMAT;
      if (strcmp(p->items[i].name, f->pet_name) == 0 &&
          (++found > 1u || !matches(f, s, &p->items[i], definition)))
        rc = H2_PAL_ERR_FORMAT;
    }
    if (rc != H2_PAL_OK)
      break;
    if (!p->has_next) {
      rc = found == 1u ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
      break;
    }
    if (!strcmp(cursor, p->next_cursor)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (strlen(p->next_cursor) >= sizeof(cursor)) {
      rc = H2_PAL_ERR_TRUNCATED;
      break;
    }
    memcpy(cursor, p->next_cursor, strlen(p->next_cursor) + 1u);
    rc = H2_PAL_ERR_NO_SPACE;
  }
  return proof(req, LIST, rc);
}
static int actions(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                   bool req, uint64_t *id, const char *definition) {
  union response r;
  int rc = call(f, s, req, ACTIONS, id, "", "", &r);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_pet_actions_t *a = &r.actions;
  if (!text(s, a->pet_name, true) || !text(s, a->pet_def_name, true) ||
      strcmp(a->pet_name, f->pet_name) || strcmp(a->pet_def_name, definition) ||
      a->clip_name_count > 256u ||
      (a->clip_name_count &&
       (!owns(s, a->clip_names, a->clip_name_count * sizeof(*a->clip_names)) ||
        (uintptr_t)a->clip_names % _Alignof(h2_gizclaw_pet_clip_name_t))))
    return proof(req, ACTIONS, H2_PAL_ERR_FORMAT);
  const char *clips[] = {a->feed, a->bathe, a->play, a->heal,
                         a->idle, a->sick,  a->dead, a->sleep};
  for (size_t i = 0u; i < sizeof(clips) / sizeof(clips[0]); ++i)
    if (!text(s, clips[i], false))
      rc = H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < a->clip_name_count && rc == H2_PAL_OK; ++i) {
    if (!text(s, a->clip_names[i].id, true) ||
        !text(s, a->clip_names[i].pixa_clip_name, true)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    for (size_t j = 0u; j < i; ++j)
      if (!strcmp(a->clip_names[i].id, a->clip_names[j].id))
        rc = H2_PAL_ERR_FORMAT;
  }
  return proof(req, ACTIONS, rc);
}
static int delete_pet(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                      bool req, uint64_t *id, const char *definition) {
  union response r;
  int rc = call(f, s, req, DELETE, id, "", "", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.pet, definition))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    f->pet_delete_acknowledged = true;
    for (unsigned attempt = 0u; attempt < 32u; ++attempt) {
      /* Independent readback uses the same Service and exact resource name. */
      rc = call(f, s, false, GET, id, "", "", &r);
      if (rc == H2_PAL_ERR_NOT_FOUND) {
        f->pet_created = f->pet_delete_acknowledged = false;
        rc = H2_PAL_OK;
        break;
      }
      if (rc != H2_PAL_OK)
        break;
      if (!matches(f, s, &r.pet, definition)) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      /* Delete acknowledges a queued server operation, not physical removal. */
      rc = attempt == 31u ? H2_PAL_ERR_TIMEOUT
                          : h2_pal_time_sleep_ms(f->time, 100u);
      if (rc != H2_PAL_OK)
        break;
    }
  }
  return proof(req, DELETE, rc);
}
static int adopt_new(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                     uint64_t *id, char *definition) {
  union response r;
  definition[0] = '\0';
  int rc = call(f, s, false, ADOPT, id, "", "", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.pet, definition))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    if (strlen(r.pet.pet_def_name) > PET_TEXT_MAX)
      rc = H2_PAL_ERR_TRUNCATED;
    else
      memcpy(definition, r.pet.pet_def_name, strlen(r.pet.pet_def_name) + 1u);
  }
  if (rc == H2_PAL_OK)
    rc = get(f, s, false, id, definition);
  return rc;
}
int h2_gizclaw_e2e_run_pet(h2_gizclaw_e2e_fixture_t *f,
                           h2_gizclaw_resp_storage_t *s) {
  if (!f || !s || !s->data || !s->capacity ||
      !f->actors[H2_GIZCLAW_E2E_OWNER].service || !f->pet_name[0] ||
      !memchr(f->pet_name, '\0', sizeof(f->pet_name)))
    return H2_PAL_ERR_INVALID_ARG;
  if (f->pet_created)
    return H2_PAL_ERR_INVALID_STATE;
  char original[sizeof(f->pet_name)];
  memcpy(original, f->pet_name, sizeof(original));
  const size_t original_len = strlen(original);
  if (original_len > sizeof(original) - sizeof("-keep"))
    return H2_PAL_ERR_TRUNCATED;
  atomic_init(&f->pet_download_bytes, 0u);
  char definition[PET_TEXT_MAX + 1u] = {0};
  uint64_t id = 200u;
  int rc = H2_PAL_OK;
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    bool req = api == 0u;
    union response r;
    /* Reusing the caller-assigned name exercises idempotent adoption. */
    rc = call(f, s, req, ADOPT, &id, "", "", &r);
    if (rc == H2_PAL_OK && !matches(f, s, &r.pet, definition))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK && !definition[0]) {
      if (strlen(r.pet.pet_def_name) >= sizeof(definition))
        rc = H2_PAL_ERR_TRUNCATED;
      else
        memcpy(definition, r.pet.pet_def_name, strlen(r.pet.pet_def_name) + 1u);
    }
    if (rc == H2_PAL_OK)
      rc = get(f, s, req, &id, definition);
    proof(req, ADOPT, rc);
    if (rc == H2_PAL_OK)
      rc = list(f, s, req, &id, definition);
    char key[H2_GIZCLAW_E2E_NAME_CAPACITY + 16u];
    (void)snprintf(key, sizeof(key), "%s-drive-%u", f->pet_name, api);
    if (rc == H2_PAL_OK)
      rc = call(f, s, req, DRIVE, &id, "", key, &r);
    if (rc == H2_PAL_OK && !matches(f, s, &r.pet, definition))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK)
      rc = get(f, s, req, &id, definition);
    proof(req, DRIVE, rc);
    if (rc == H2_PAL_OK)
      rc = actions(f, s, req, &id, definition);
    if (rc == H2_PAL_OK)
      rc = call(f, s, req, PIXA, &id, "", "", &r);
    if (rc == H2_PAL_OK &&
        (!text(s, r.pixa.pet_name, true) ||
         !text(s, r.pixa.pet_def_name, true) ||
         !text(s, r.pixa.source_path, true) ||
         strcmp(r.pixa.pet_name, f->pet_name) ||
         strcmp(r.pixa.pet_def_name, definition) || !r.pixa.size_bytes ||
         r.pixa.size_bytes != r.pixa.received_bytes ||
         r.pixa.received_bytes != atomic_load(&f->pet_download_bytes)))
      rc = H2_PAL_ERR_FORMAT;
    proof(req, PIXA, rc);
  }
  if (rc == H2_PAL_OK)
    rc = delete_pet(f, s, true, &id, definition);
  if (rc == H2_PAL_OK) {
    /* Never reuse a deleted adoption reservation, or overwrite a pending name.
     */
    memcpy(f->pet_name, original, original_len);
    memcpy(f->pet_name + original_len, "-rpc", sizeof("-rpc"));
    rc = adopt_new(f, s, &id, definition);
  }
  if (rc == H2_PAL_OK)
    rc = delete_pet(f, s, false, &id, definition);
  if (rc == H2_PAL_OK) {
    /* The later same-name isolation domain needs a live owner Pet. */
    memcpy(f->pet_name, original, original_len);
    memcpy(f->pet_name + original_len, "-keep", sizeof("-keep"));
    rc = adopt_new(f, s, &id, definition);
  }
  s->used = 0u;
  return rc;
}
