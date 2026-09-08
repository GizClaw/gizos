#include "h2_gizclaw_e2e_resource.h"
#include <stdio.h>
#include <string.h>

#define RESOURCE_TIMEOUT_MS 30000u
#define RESOURCE_BYTES 65536u
#define RESOURCE_ITEMS 64u

typedef struct resource_case {
  h2_gizclaw_resource_t *store;
  uint8_t *scratch;
} resource_case_t;

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
static int proof(const char *symbol, const char *stage, bool ok) {
  return checked(symbol, stage, ok ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE);
}
static int dispose(h2_gizclaw_e2e_fixture_t *f) {
  resource_case_t *s = f->case_state;
  if (s == NULL)
    return H2_PAL_OK;
  if (s->store != NULL) {
    int rc = checked("h2_gizclaw_resource_close", "resource-cleanup",
                     h2_gizclaw_resource_close(s->store));
    if (rc != H2_PAL_OK)
      return rc;
    rc = checked("h2_gizclaw_resource_destroy", "resource-cleanup",
                 h2_gizclaw_resource_destroy(&s->store));
    if (rc != H2_PAL_OK)
      return rc;
    rc = proof("h2_gizclaw_resource_destroy", "resource_destroy-assert",
               s->store == NULL);
    if (rc != H2_PAL_OK)
      return rc;
  }
  h2_pal_mem_free(f->allocator, s->scratch);
  h2_pal_mem_free(f->allocator, s);
  f->case_state = NULL;
  f->case_cleanup = NULL;
  return H2_PAL_OK;
}
static int snapshot(resource_case_t *s, h2_gizclaw_resource_snapshot_t *out) {
  h2_gizclaw_resp_storage_t storage = {s->scratch, RESOURCE_BYTES, 0u};
  return checked("h2_gizclaw_resource_snapshot", "resource-snapshot",
                 h2_gizclaw_resource_snapshot(s->store, &storage, out));
}
static int execute(h2_gizclaw_e2e_fixture_t *f, resource_case_t *s,
                   const h2_gizclaw_resource_command_t *command,
                   h2_gizclaw_resource_snapshot_t *out) {
  if (!h2_gizclaw_e2e_fixture_has_time(f, RESOURCE_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  int rc = checked(
      "h2_gizclaw_resource_execute", "resource-execute",
      h2_gizclaw_resource_execute(s->store, command, RESOURCE_TIMEOUT_MS));
  if (rc == H2_PAL_OK)
    rc = snapshot(s, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_snapshot", "resource_snapshot-assert",
               out->valid && !out->stale && !out->busy && !out->closed &&
                   out->last_error == H2_PAL_OK && out->data_revision != 0u);
  return rc;
}
static bool contact_matches(const h2_gizclaw_resource_snapshot_t *out,
                            const char *name, const char *display) {
  size_t found = 0u;
  for (size_t i = 0; i < out->data.contacts.count; ++i) {
    const h2_gizclaw_contact_t *c = &out->data.contacts.items[i];
    if (c->name != NULL && !strcmp(c->name, name)) {
      if (display == NULL || c->display_name == NULL ||
          c->phone_number == NULL || strcmp(c->display_name, display) ||
          strcmp(c->phone_number, "+12025550123"))
        return false;
      ++found;
    }
  }
  return display == NULL ? found == 0u : found == 1u;
}
static int contacts(h2_gizclaw_e2e_fixture_t *f, resource_case_t *s,
                    h2_gizclaw_resource_snapshot_t *out) {
  int n = snprintf(f->contact_name, sizeof(f->contact_name), "%s-resource",
                   f->run_prefix);
  if (n <= 0 || (size_t)n >= sizeof(f->contact_name))
    return H2_PAL_ERR_NO_SPACE;
  h2_gizclaw_resource_command_t cmd = {.operation =
                                           H2_GIZCLAW_RESOURCE_CONTACT_CREATE,
                                       .name = f->contact_name,
                                       .display_name = "Resource before",
                                       .phone_number = "+12025550123"};
  /* Keep deletion ownership even if a successful remote mutation loses its
   * reply. */
  f->contact_created = true;
  int rc = execute(f, s, &cmd, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource-contact-create-assert",
               contact_matches(out, f->contact_name, cmd.display_name));
  /* A separate arena must survive the next snapshot commit. */
  h2_gizclaw_resp_storage_t saved_storage = {s->scratch + RESOURCE_BYTES,
                                             RESOURCE_BYTES, 0u};
  h2_gizclaw_resource_snapshot_t saved = {0};
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resource_snapshot(s->store, &saved_storage, &saved);
  const uint64_t previous = out->data_revision;
  cmd.operation = H2_GIZCLAW_RESOURCE_CONTACT_UPDATE;
  cmd.display_name = "Resource after";
  if (rc == H2_PAL_OK)
    rc = execute(f, s, &cmd, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource-contact-update-assert",
               out->data_revision > previous &&
                   contact_matches(out, f->contact_name, cmd.display_name) &&
                   contact_matches(&saved, f->contact_name, "Resource before"));
  cmd.operation = H2_GIZCLAW_RESOURCE_CONTACT_DELETE;
  if (rc == H2_PAL_OK)
    rc = execute(f, s, &cmd, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource-contact-delete-assert",
               contact_matches(out, f->contact_name, NULL));
  if (rc == H2_PAL_OK)
    f->contact_created = false;
  return rc;
}
static int profile(h2_gizclaw_e2e_fixture_t *f, resource_case_t *s,
                   h2_gizclaw_resource_snapshot_t *out) {
  h2_gizclaw_resource_command_t cmd = {
      .operation = H2_GIZCLAW_RESOURCE_PROFILE_NAME, .text = f->run_prefix};
  int rc = execute(f, s, &cmd, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource-profile-name-assert",
               out->data.profile.has_name &&
                   !strcmp(out->data.profile.name, cmd.text));
  cmd.operation = H2_GIZCLAW_RESOURCE_PROFILE_EMOJI;
  cmd.text = "🧪";
  if (rc == H2_PAL_OK)
    rc = execute(f, s, &cmd, out);
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource-profile-emoji-assert",
               out->data.profile.has_name &&
                   !strcmp(out->data.profile.name, f->run_prefix) &&
                   out->data.profile.has_emoji &&
                   !strcmp(out->data.profile.emoji, cmd.text));
  return rc;
}
static int run_kind(h2_gizclaw_e2e_fixture_t *f, resource_case_t *s,
                    h2_gizclaw_resource_kind_t kind) {
  h2_gizclaw_resource_config_t config = {
      .kind = kind,
      .service = f->actors[H2_GIZCLAW_E2E_OWNER].service,
      .mem = f->allocator,
      .sync = f->runtime->sync,
      .time = f->time,
      .runtime = f->runtime,
      .max_items = RESOURCE_ITEMS,
      .page_size = 1u,
      .storage_bytes = RESOURCE_BYTES};
  int rc = checked("h2_gizclaw_resource_create", "resource-create",
                   h2_gizclaw_resource_create(&config, &s->store));
  h2_gizclaw_resource_snapshot_t out = {0};
  if (rc == H2_PAL_OK)
    rc = snapshot(s, &out);
  if (rc == H2_PAL_OK)
    rc =
        proof("h2_gizclaw_resource_create", "resource_create-assert",
              s->store != NULL && out.kind == kind && !out.valid && out.stale &&
                  !out.busy && !out.closed && out.data_revision == 0u);
  h2_gizclaw_resource_command_t cmd = {.operation =
                                           H2_GIZCLAW_RESOURCE_REFRESH};
  if (rc == H2_PAL_OK)
    rc = execute(f, s, &cmd, &out);
  if (rc == H2_PAL_OK && kind == H2_GIZCLAW_RESOURCE_CONTACTS)
    rc = contacts(f, s, &out);
  if (rc == H2_PAL_OK && kind == H2_GIZCLAW_RESOURCE_PROFILE)
    rc = profile(f, s, &out);
  if (rc == H2_PAL_OK && kind == H2_GIZCLAW_RESOURCE_GROUPS)
    rc = proof("h2_gizclaw_resource_execute", "resource-groups-assert",
               !out.data.groups.has_next && (out.data.groups.count == 0u ||
                                             out.data.groups.items != NULL));
  if (rc != H2_PAL_OK)
    return rc;
  const uint64_t revision = out.data_revision;
  rc = checked("h2_gizclaw_resource_close", "resource-close",
               h2_gizclaw_resource_close(s->store));
  if (rc == H2_PAL_OK)
    rc = snapshot(s, &out);
  if (rc == H2_PAL_OK)
    rc = proof(
        "h2_gizclaw_resource_close", "resource_close-assert",
        out.closed && out.stale && !out.busy && out.valid &&
            out.data_revision == revision &&
            h2_gizclaw_resource_execute(s->store, &cmd, RESOURCE_TIMEOUT_MS) ==
                H2_PAL_ERR_CLOSED);
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_resource_destroy", "resource-destroy",
                 h2_gizclaw_resource_destroy(&s->store));
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_destroy", "resource_destroy-assert",
               s->store == NULL);
  return rc;
}
int h2_gizclaw_e2e_run_resource(h2_gizclaw_e2e_fixture_t *f) {
  if (f == NULL || f->runtime == NULL || f->allocator == NULL ||
      f->actors[H2_GIZCLAW_E2E_OWNER].service == NULL ||
      f->case_state != NULL || f->case_cleanup != NULL || f->contact_created)
    return H2_PAL_ERR_INVALID_ARG;
  resource_case_t *s = h2_pal_mem_alloc(f->allocator, sizeof(*s));
  if (s == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  f->case_state = s;
  f->case_cleanup = dispose;
  s->scratch = h2_pal_mem_alloc(f->allocator, 2u * RESOURCE_BYTES);
  int rc = s->scratch == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
  for (int kind = H2_GIZCLAW_RESOURCE_CONTACTS;
       kind <= H2_GIZCLAW_RESOURCE_GROUPS && rc == H2_PAL_OK; ++kind) {
    printf("H2_GIZCLAW_E2E stage=resource-kind kind=%d\n", kind);
    rc = run_kind(f, s, (h2_gizclaw_resource_kind_t)kind);
  }
  if (rc == H2_PAL_OK)
    rc = proof("h2_gizclaw_resource_execute", "resource_execute-assert", true);
  int cleanup_rc = dispose(f);
  return rc != H2_PAL_OK ? rc : cleanup_rc;
}
