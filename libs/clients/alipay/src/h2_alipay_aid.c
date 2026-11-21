#include "h2_alipay_client.h"

#include "h2_alipay_client_internal.h"
#include "yyjson.h"
#include <stdio.h>
#include <string.h>

h2_pal_result_t h2_alipay_client_get_aid_code(
    const h2_alipay_client_config_t* config, bool ble, char* out,
    size_t* length) {
  if (out == NULL || length == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  if (strlen(config->identity.model) >= 32u || strlen(config->company) >= 24u)
    return H2_PAL_ERR_INVALID_STATE;
  char model[50], mac[24], timestamp[16];
  int written = snprintf(model, sizeof(model), "%s.%s", config->company,
                         config->identity.model);
  if (written < 0 || strlen(config->identity.mac) >= sizeof(mac))
    return H2_PAL_ERR_NO_SPACE;
  memcpy(mac, config->identity.mac, strlen(config->identity.mac) + 1u);
  for (size_t index = 0u; mac[index] != '\0'; ++index)
    if (mac[index] >= 'a' && mac[index] <= 'z')
      mac[index] = (char)(mac[index] - 'a' + 'A');
  uint64_t milliseconds = 0u;
  rc = h2_pal_time_get_wall_ms(config->time, &milliseconds);
  if (rc != H2_PAL_OK) return rc;
  uint64_t seconds = milliseconds / 1000u;
  if (seconds > UINT32_MAX) return H2_PAL_ERR_INVALID_STATE;
  int64_t signed_seconds = seconds <= INT32_MAX
                               ? (int64_t)seconds
                               : (int64_t)seconds - INT64_C(4294967296);
  written =
      snprintf(timestamp, sizeof(timestamp), "%lld", (long long)signed_seconds);
  if (written < 0 || (size_t)written >= sizeof(timestamp))
    return H2_PAL_ERR_FORMAT;
  yyjson_alc allocator = {h2_alipay_client_json_alloc,
                          h2_alipay_client_json_realloc,
                          h2_alipay_client_json_free, (void*)config->mem};
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* object = yyjson_mut_obj(document);
  bool ok =
      object != NULL && yyjson_mut_obj_add_str(document, object, "mac", mac) &&
      yyjson_mut_obj_add_str(document, object, "gtime", timestamp) &&
      yyjson_mut_obj_add_str(document, object, "pmodel", model) &&
      yyjson_mut_obj_add_str(document, object, "ver", "4.61") &&
      yyjson_mut_obj_add_str(document, object, "type", ble ? "ble" : "net");
  yyjson_mut_doc_set_root(document, object);
  size_t size = 0u;
  char* json =
      ok ? yyjson_mut_write_opts(document, 0u, &allocator, &size, NULL) : NULL;
  rc = H2_PAL_ERR_NO_MEMORY;
  if (json != NULL) {
    static const char prefix[] = "ALIPAY_HPWT#0#";
    size_t required = sizeof(prefix) + size;
    if (*length < required) {
      *length = required;
      rc = H2_PAL_ERR_NO_SPACE;
    } else {
      memcpy(out, prefix, sizeof(prefix) - 1u);
      memcpy(out + sizeof(prefix) - 1u, json, size + 1u);
      *length = required - 1u;
      rc = H2_PAL_OK;
    }
  }
  h2_alipay_client_json_free((void*)config->mem, json);
  yyjson_mut_doc_free(document);
  return rc;
}
