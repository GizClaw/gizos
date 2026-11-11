#include "h2_alipay_tiga_identity.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_modem.h"
#include "h2_app_test_pref.h"

#include <assert.h>
#include <string.h>

static void store(const h2_pal_pref_api_t* pref, const char* value) {
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(pref, "h106_factory", H2_PAL_PREF_OPEN_READ_WRITE,
                          &namespace) == H2_PAL_OK);
  assert(namespace->set_string(namespace, "imei", value) == H2_PAL_OK);
  assert(namespace->commit(namespace) == H2_PAL_OK);
  assert(namespace->close(namespace) == H2_PAL_OK);
}

int main(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == H2_PAL_OK);
  h2_app_test_modem_t modem;
  h2_app_test_modem_init(&modem);
  strcpy(modem.identity.imei, "490154203237518");
  h2_runtime_t runtime = {
      .pref = &pref.api, .mem = &memory.api, .modem = &modem.api};
  char value[64] = "stale";
  assert(h2_alipay_tiga_read_sn(&runtime, value, sizeof(value)) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(value[0] == '\0' && pref.active_handles == 0u);
  store(&pref.api, "123456789012347");
  assert(h2_alipay_tiga_read_sn(&runtime, value, sizeof(value)) == H2_PAL_OK);
  assert(strcmp(value, "123456789012347") == 0 && pref.active_handles == 0u);
  assert(h2_alipay_tiga_read_imei(&runtime, value, sizeof(value)) == H2_PAL_OK);
  assert(strcmp(value, modem.identity.imei) == 0);
  assert(h2_alipay_tiga_read_sn(&runtime, value, 15u) == H2_PAL_ERR_NO_SPACE);
  assert(value[0] == '\0' && pref.active_handles == 0u);
  store(&pref.api, "123456789012348");
  assert(h2_alipay_tiga_read_sn(&runtime, value, sizeof(value)) ==
         H2_PAL_ERR_FORMAT);
  assert(value[0] == '\0' && pref.active_handles == 0u);
  store(&pref.api, "123456789012347");
  pref.read = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(h2_alipay_tiga_read_sn(&runtime, value, sizeof(value)) ==
         H2_PAL_ERR_IO);
  assert(pref.active_handles == 0u && value[0] == '\0');
  modem.get_identity =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_TIMEOUT, .remaining = 1u};
  assert(h2_alipay_tiga_read_imei(&runtime, value, sizeof(value)) ==
         H2_PAL_ERR_TIMEOUT);
  assert(value[0] == '\0');
  memset(modem.identity.imei, '9', sizeof(modem.identity.imei));
  assert(h2_alipay_tiga_read_imei(&runtime, value, sizeof(value)) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(value[0] == '\0');
  assert(h2_alipay_tiga_read_sn(NULL, value, sizeof(value)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(memory.live_blocks == 0u);
  return 0;
}
