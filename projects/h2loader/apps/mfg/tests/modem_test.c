#include "../src/h2_mfg_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fixture {
  h2_mfg_t m;
  h2_runtime_t runtime;
  h2_mfg_profile_t profile;
  h2_mfg_step_t step;
  h2_pal_modem_api_t api;
  unsigned opens, identities, closes;
  int owned, open_error, close_error, identity_error;
  int missing_model, missing_identity;
} fixture_t;

static int open_modem(void* user, uint32_t timeout) {
  fixture_t* f = user;
  assert(timeout != 0 && timeout <= 30000 && !f->owned);
  ++f->opens;
  if (f->open_error == H2_PAL_ERR_UNSUPPORTED) return f->open_error;
  f->owned = 1; /* Even a failed native open can retain cleanup ownership. */
  return f->open_error;
}
static int close_modem(void* user, uint32_t timeout) {
  fixture_t* f = user;
  assert(timeout != 0 && timeout <= 30000 && f->owned);
  ++f->closes;
  if (f->close_error) return f->close_error;
  f->owned = 0;
  return H2_PAL_OK;
}
static int identity(void* user, h2_pal_modem_identity_t* out) {
  fixture_t* f = user;
  assert(f->owned && f->opens == 1);
  ++f->identities;
  if (f->identity_error) return f->identity_error;
  memset(out, 0, sizeof(*out));
  if (!f->missing_model) strcpy(out->model, "fixture-model");
  if (!f->missing_identity) strcpy(out->imei, "000000000000000");
  return H2_PAL_OK;
}
static const h2_pal_modem_vtable_t modem_ops = {
    .open = open_modem, .close = close_modem, .get_identity = identity};

static void setup(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  f->api = (h2_pal_modem_api_t){.user = f, .vtable = &modem_ops};
  f->runtime.modem = &f->api;
  f->step = (h2_mfg_step_t){"modem", H2_MFG_MODEM, 1};
  f->profile =
      (h2_mfg_profile_t){.board = "test", .steps = &f->step, .total = 1};
  f->m = (h2_mfg_t){.rt = &f->runtime,
                    .profile = &f->profile,
                    .model = {.profile = &f->profile, .summary = {.total = 1}}};
}

int main(void) {
  fixture_t f;
  setup(&f);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(f.opens == 1 && f.m.modem_owned && f.identities == 0);
  const char* action = NULL;
  assert(h2_mfg_hardware_tick(&f.m, 20, &action) == 0);
  assert(f.m.model.ready && action && strcmp(action, "confirm") == 0);
  f.missing_model = 1;
  action = NULL;
  assert(h2_mfg_hardware_tick(&f.m, 40, &action) == 0);
  assert(!f.m.model.ready && action == NULL);
  f.missing_model = 0;
  f.missing_identity = 1;
  assert(h2_mfg_hardware_tick(&f.m, 60, &action) == 0);
  assert(!f.m.model.ready && action == NULL && f.opens == 1);
  f.identity_error = H2_PAL_ERR_CLOSED;
  assert(h2_mfg_hardware_tick(&f.m, 80, &action) == H2_PAL_ERR_CLOSED);
  assert(!f.m.model.ready && action == NULL);
  f.close_error = H2_PAL_ERR_BUSY;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.modem_owned && f.owned && f.closes == 1);
  f.close_error = 0;
  assert(h2_mfg_hardware_close(&f.m) == 0);
  assert(!f.m.modem_owned && !f.owned && f.closes == 2);
  assert(h2_mfg_hardware_close(&f.m) == 0 && f.closes == 2);

  setup(&f);
  f.open_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_TIMEOUT);
  assert(f.m.modem_owned && f.owned && f.identities == 0);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  assert(!f.m.modem_owned && !f.owned && f.closes == 1);

  setup(&f);
  f.open_error = H2_PAL_ERR_UNSUPPORTED;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_UNSUPPORTED);
  assert(!f.m.modem_owned && !f.owned && f.identities == 0);
  assert(h2_mfg_hardware_close(&f.m) == 0 && f.closes == 0);

  setup(&f);
  h2_pal_modem_vtable_t no_close = modem_ops;
  no_close.close = NULL;
  f.api.vtable = &no_close;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_UNSUPPORTED);
  assert(f.opens == 0 && !f.m.modem_owned);
  return 0;
}
