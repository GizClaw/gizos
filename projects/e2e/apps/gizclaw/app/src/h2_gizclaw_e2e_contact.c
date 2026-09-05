#include "h2_gizclaw_e2e_contact.h"

#include <string.h>

#define CONTACT_TIMEOUT_MS 30000u
#define CONTACT_LIMIT 32u
#define CONTACT_MAX_PAGES 32u

enum method {
  CONTACT_CREATE,
  CONTACT_GET,
  CONTACT_PUT,
  CONTACT_LIST,
  CONTACT_DELETE
};
static const char *const assertions[] = {
    "contact_create-assert", "contact_get-assert", "contact_put-assert",
    "contact_list-assert", "contact_delete-assert"};
static const char *const rpc_symbols[] = {
    "h2_gizclaw_rpc_contact_create", "h2_gizclaw_rpc_contact_get",
    "h2_gizclaw_rpc_contact_put", "h2_gizclaw_rpc_contact_list",
    "h2_gizclaw_rpc_contact_delete"};
static const char *const parse_symbols[] = {
    "h2_gizclaw_resp_parse_contact_create", "h2_gizclaw_resp_parse_contact_get",
    "h2_gizclaw_resp_parse_contact_put", "h2_gizclaw_resp_parse_contact_list",
    "h2_gizclaw_resp_parse_contact_delete"};

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static int proof(bool req_api, enum method method, int rc) {
  return checked(req_api ? parse_symbols[method] : rpc_symbols[method],
                 assertions[method], rc);
}

static int call(h2_gizclaw_e2e_fixture_t *fixture,
                h2_gizclaw_resp_storage_t *storage, bool req_api,
                enum method method, uint64_t *identity, const char *display,
                const char *phone, const char *cursor,
                h2_gizclaw_contact_t *contact,
                h2_gizclaw_contact_page_t *page) {
  storage->used = 0u;
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, CONTACT_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  const h2_gizclaw_str_t name = h2_gizclaw_e2e_str(fixture->contact_name);
  const h2_gizclaw_str_t display_value = h2_gizclaw_e2e_str(display);
  const h2_gizclaw_str_t phone_value = h2_gizclaw_e2e_str(phone);
  const h2_gizclaw_str_t cursor_value = h2_gizclaw_e2e_str(cursor);
  int rc = H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  if (req_api) {
    const uint64_t id = (*identity)++;
    switch (method) {
    case CONTACT_CREATE:
      rc = checked("h2_gizclaw_req_create_contact_create", "contact-req",
                   h2_gizclaw_req_create_contact_create(
                       service, id, name, display_value, phone_value,
                       CONTACT_TIMEOUT_MS, &request));
      break;
    case CONTACT_GET:
      rc = checked("h2_gizclaw_req_create_contact_get", "contact-req",
                   h2_gizclaw_req_create_contact_get(
                       service, id, name, CONTACT_TIMEOUT_MS, &request));
      break;
    case CONTACT_PUT:
      rc = checked("h2_gizclaw_req_create_contact_put", "contact-req",
                   h2_gizclaw_req_create_contact_put(
                       service, id, name, display_value, phone_value,
                       CONTACT_TIMEOUT_MS, &request));
      break;
    case CONTACT_LIST:
      rc = checked("h2_gizclaw_req_create_contact_list", "contact-req",
                   h2_gizclaw_req_create_contact_list(
                       service, id, cursor_value, CONTACT_LIMIT,
                       CONTACT_TIMEOUT_MS, &request));
      break;
    case CONTACT_DELETE:
      rc = checked("h2_gizclaw_req_create_contact_delete", "contact-req",
                   h2_gizclaw_req_create_contact_delete(
                       service, id, name, CONTACT_TIMEOUT_MS, &request));
      break;
    }
    if (rc == H2_PAL_OK) {
      // Constructors do not send. Record ownership immediately before do.
      if (method == CONTACT_CREATE)
        fixture->contact_created = true;
      rc = checked("h2_gizclaw_req_do", "contact-req",
                   h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
    }
    if (rc == H2_PAL_OK)
      rc = checked("h2_gizclaw_req_wait", "contact-req",
                   h2_gizclaw_req_wait(request, CONTACT_TIMEOUT_MS));
    if (rc == H2_PAL_OK) {
      switch (method) {
      case CONTACT_CREATE:
        rc = checked(
            parse_symbols[method], "contact-req",
            h2_gizclaw_resp_parse_contact_create(request, storage, contact));
        break;
      case CONTACT_GET:
        rc = checked(
            parse_symbols[method], "contact-req",
            h2_gizclaw_resp_parse_contact_get(request, storage, contact));
        break;
      case CONTACT_PUT:
        rc = checked(
            parse_symbols[method], "contact-req",
            h2_gizclaw_resp_parse_contact_put(request, storage, contact));
        break;
      case CONTACT_LIST:
        rc =
            checked(parse_symbols[method], "contact-req",
                    h2_gizclaw_resp_parse_contact_list(request, storage, page));
        break;
      case CONTACT_DELETE:
        rc = checked(
            parse_symbols[method], "contact-req",
            h2_gizclaw_resp_parse_contact_delete(request, storage, contact));
        break;
      }
    }
    if (request != NULL && rc != H2_PAL_OK)
      (void)checked("h2_gizclaw_req_cancel", "contact-cleanup",
                    h2_gizclaw_req_cancel(request));
    h2_gizclaw_req_release(request);
  } else {
    if (method == CONTACT_CREATE)
      fixture->contact_created = true;
    switch (method) {
    case CONTACT_CREATE:
      rc = checked(rpc_symbols[method], "contact-rpc",
                   h2_gizclaw_rpc_contact_create(
                       service, name, display_value, phone_value,
                       CONTACT_TIMEOUT_MS, storage, contact));
      break;
    case CONTACT_GET:
      rc = checked(rpc_symbols[method], "contact-rpc",
                   h2_gizclaw_rpc_contact_get(service, name, CONTACT_TIMEOUT_MS,
                                              storage, contact));
      break;
    case CONTACT_PUT:
      rc = checked(rpc_symbols[method], "contact-rpc",
                   h2_gizclaw_rpc_contact_put(service, name, display_value,
                                              phone_value, CONTACT_TIMEOUT_MS,
                                              storage, contact));
      break;
    case CONTACT_LIST:
      rc = checked(
          rpc_symbols[method], "contact-rpc",
          h2_gizclaw_rpc_contact_list(service, cursor_value, CONTACT_LIMIT,
                                      CONTACT_TIMEOUT_MS, storage, page));
      break;
    case CONTACT_DELETE:
      rc = checked(rpc_symbols[method], "contact-rpc",
                   h2_gizclaw_rpc_contact_delete(
                       service, name, CONTACT_TIMEOUT_MS, storage, contact));
      break;
    }
  }
  return rc;
}

static bool matches(const h2_gizclaw_contact_t *contact, const char *name,
                    const char *display, const char *phone) {
  return contact->name != NULL && strcmp(contact->name, name) == 0 &&
         contact->display_name != NULL &&
         strcmp(contact->display_name, display) == 0 &&
         contact->phone_number != NULL &&
         strcmp(contact->phone_number, phone) == 0;
}

static int object_call(h2_gizclaw_e2e_fixture_t *fixture,
                       h2_gizclaw_resp_storage_t *storage, bool req_api,
                       enum method method, uint64_t *identity,
                       const char *display, const char *phone) {
  h2_gizclaw_contact_t contact = {0};
  int rc = call(fixture, storage, req_api, method, identity, display, phone, "",
                &contact, NULL);
  if (rc == H2_PAL_OK) {
    const bool valid =
        method == CONTACT_DELETE
            ? contact.name != NULL &&
                  strcmp(contact.name, fixture->contact_name) == 0
            : matches(&contact, fixture->contact_name, display, phone);
    if (!valid)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  storage->used = 0u;
  if (rc == H2_PAL_OK && method == CONTACT_DELETE)
    fixture->contact_created = false; // A validated delete acknowledgment.
  return rc;
}

static int verify_list(h2_gizclaw_e2e_fixture_t *fixture,
                       h2_gizclaw_resp_storage_t *storage, bool req_api,
                       uint64_t *identity, const char *display,
                       const char *phone, bool present) {
  char cursor[H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES + 1u] = {0};
  size_t found = 0u;
  for (unsigned page_index = 0u; page_index < CONTACT_MAX_PAGES; ++page_index) {
    h2_gizclaw_contact_page_t page = {0};
    int rc = call(fixture, storage, req_api, CONTACT_LIST, identity, display,
                  phone, cursor, NULL, &page);
    if (rc == H2_PAL_OK && (page.count > CONTACT_LIMIT ||
                            (page.count != 0u && page.items == NULL)))
      rc = H2_PAL_ERR_INVALID_STATE;
    for (size_t i = 0u; i < page.count && rc == H2_PAL_OK; ++i) {
      const h2_gizclaw_contact_t *contact = &page.items[i];
      if (contact->name == NULL || contact->name[0] == '\0') {
        rc = H2_PAL_ERR_INVALID_STATE;
      } else if (strcmp(contact->name, fixture->contact_name) == 0) {
        ++found;
        // A supposedly deleted Contact still exists: restore cleanup duty.
        fixture->contact_created = true;
        if (!present || found > 1u ||
            !matches(contact, fixture->contact_name, display, phone))
          rc = H2_PAL_ERR_INVALID_STATE;
      }
    }
    if (rc == H2_PAL_OK && page.has_next) {
      if (page.next_cursor == NULL || page.next_cursor[0] == '\0' ||
          strlen(page.next_cursor) >= sizeof(cursor) ||
          strcmp(cursor, page.next_cursor) == 0) {
        rc = H2_PAL_ERR_INVALID_STATE;
      } else {
        memcpy(cursor, page.next_cursor, strlen(page.next_cursor) + 1u);
      }
    }
    storage->used = 0u;
    if (rc != H2_PAL_OK)
      return proof(req_api, CONTACT_LIST, rc);
    if (!page.has_next)
      return proof(req_api, CONTACT_LIST,
                   found == (present ? 1u : 0u) ? H2_PAL_OK
                                                : H2_PAL_ERR_INVALID_STATE);
  }
  return proof(req_api, CONTACT_LIST, H2_PAL_ERR_NO_SPACE);
}

static int create_and_read(h2_gizclaw_e2e_fixture_t *fixture,
                           h2_gizclaw_resp_storage_t *storage, bool req_api,
                           uint64_t *identity, const char *display,
                           const char *phone) {
  int rc = object_call(fixture, storage, req_api, CONTACT_CREATE, identity,
                       display, phone);
  if (rc == H2_PAL_OK)
    rc = proof(req_api, CONTACT_GET,
               object_call(fixture, storage, req_api, CONTACT_GET, identity,
                           display, phone));
  return proof(req_api, CONTACT_CREATE, rc);
}

int h2_gizclaw_e2e_run_contact(h2_gizclaw_e2e_fixture_t *fixture,
                               h2_gizclaw_resp_storage_t *storage) {
  if (fixture == NULL || storage == NULL || storage->data == NULL ||
      storage->capacity == 0u ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (fixture->contact_created || fixture->isolation_contact_pending)
    return H2_PAL_ERR_INVALID_STATE;
  const char *end =
      memchr(fixture->run_prefix, '\0', sizeof(fixture->run_prefix));
  if (end == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t len = (size_t)(end - fixture->run_prefix);
  if (len == 0u || len + sizeof("-contact") > sizeof(fixture->contact_name))
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(fixture->contact_name, fixture->run_prefix, len);
  memcpy(fixture->contact_name + len, "-contact", sizeof("-contact"));
  uint64_t identity = 31u;
  int rc = H2_PAL_OK;
  for (unsigned req_api = 0u; req_api < 2u && rc == H2_PAL_OK; ++req_api) {
    const char *display = req_api ? "request contact" : "rpc contact";
    const char *updated = req_api ? "request updated" : "rpc updated";
    const char *phone = "+8613900000644", *updated_phone = "+8613900000645";
    rc = create_and_read(fixture, storage, req_api, &identity, display, phone);
    if (rc == H2_PAL_OK)
      rc = object_call(fixture, storage, req_api, CONTACT_PUT, &identity,
                       updated, updated_phone);
    if (rc == H2_PAL_OK)
      rc = proof(req_api, CONTACT_GET,
                 object_call(fixture, storage, req_api, CONTACT_GET, &identity,
                             updated, updated_phone));
    if (rc == H2_PAL_OK)
      rc = proof(req_api, CONTACT_PUT, H2_PAL_OK);
    if (rc == H2_PAL_OK)
      rc = verify_list(fixture, storage, req_api, &identity, updated,
                       updated_phone, true);
    if (rc == H2_PAL_OK)
      rc = object_call(fixture, storage, req_api, CONTACT_DELETE, &identity,
                       updated, updated_phone);
    if (rc == H2_PAL_OK) {
      rc = verify_list(fixture, storage, req_api, &identity, updated,
                       updated_phone, false);
      rc = proof(req_api, CONTACT_DELETE, rc);
    }
  }
  // The subsequent peer-name-isolation case needs one surviving owner Contact.
  if (rc == H2_PAL_OK)
    rc = create_and_read(fixture, storage, true, &identity, "owner contact",
                         "+8613900000644");
  storage->used = 0u;
  return rc;
}
