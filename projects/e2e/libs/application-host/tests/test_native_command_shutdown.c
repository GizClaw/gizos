/* Execute complete ESP/BK production owners with faulting transport and mutex
 * PAL. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef H2_PAL_E2E_ESP
#include "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_ble/src/h2_esp_h2loader_ble.c"
#define stop_owner h2_esp_h2loader_app_commands_stop_serial
#else
#include "projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/src/h2_bk_h2loader_app_bleikcp.c"
#define stop_owner h2_bk_h2loader_stop_app_commands
#endif
static int transport_error, mutex_error, stops, destroyed, mutex_cookie;
static h2_pal_mutex_t *const mutex = (h2_pal_mutex_t *)&mutex_cookie;
static h2_pal_result_t destroy_mutex(void *user, h2_pal_mutex_t *value) {
  (void)user;
  assert(value == mutex && transport_error == H2_PAL_OK && stops > 0);
  ++destroyed;
  return mutex_error;
}
static const h2_pal_sync_vtable_t sync_vtable = {.destroy_mutex =
                                                     destroy_mutex};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};
static h2_runtime_t runtime = {.sync = &sync_api};
#ifdef H2_PAL_E2E_ESP
int h2_esp_h2loader_app_iostreamikcp_stop(void) {
  ++stops;
  return transport_error;
}
#else
int h2_bk_h2loader_stop_app_iostreamikcp(void) {
  ++stops;
  return transport_error;
}
#endif
int main(void) {
  s_ble.service = (h2_loader_ble_service_t *)&runtime;
  s_ble.operation_mutex = mutex;
  s_ble.runtime = &runtime;
#ifdef H2_PAL_E2E_ESP
  s_ble.operation_sync = &sync_api;
  s_serial_started = 1;
#endif
  assert(stop_owner() == H2_PAL_ERR_BUSY && stops == 0 && destroyed == 0);
  s_ble.service = NULL;
  transport_error = H2_PAL_ERR_TIMEOUT;
  assert(stop_owner() == H2_PAL_ERR_TIMEOUT && destroyed == 0 &&
         s_ble.operation_mutex == mutex);
  transport_error = H2_PAL_OK;
  mutex_error = H2_PAL_ERR_BUSY;
  assert(stop_owner() == H2_PAL_ERR_BUSY && s_ble.operation_mutex == mutex &&
         s_ble.runtime == &runtime);
  mutex_error = H2_PAL_OK;
  assert(stop_owner() == H2_PAL_OK && !s_ble.operation_mutex && !s_ble.runtime);
#ifdef H2_PAL_E2E_ESP
  assert(!s_ble.operation_sync && !h2_esp_h2loader_app_serial_is_started());
#endif
  assert(stop_owner() == H2_PAL_OK && destroyed == 2);
  return 0;
}
