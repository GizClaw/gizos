#include "h2_alipay_tiga_identity.h"

#include <string.h>

static bool valid_sn(const char* value) {
  if (strlen(value) != 15u) return false;
  unsigned sum = 0u;
  for (size_t index = 0u; index < 15u; ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
    unsigned digit = (unsigned)(value[index] - '0');
    if (index % 2u == 1u) {
      digit *= 2u;
      if (digit > 9u) digit -= 9u;
    }
    sum += digit;
  }
  return sum % 10u == 0u;
}

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity) {
  h2_runtime_t* runtime = user;
  if (runtime == NULL || out == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  h2_pal_pref_namespace_t* namespace = NULL;
  h2_pal_result_t result = h2_pal_pref_open(
      runtime->pref, "h106_factory", H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (result != H2_PAL_OK) return result;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  char* value = NULL;
  result = namespace->get_string != NULL
               ? namespace->get_string(namespace, runtime->mem, "imei", &value)
               : H2_PAL_ERR_UNSUPPORTED;
  if (result == H2_PAL_OK) {
    if (value == NULL || !valid_sn(value))
      result = H2_PAL_ERR_FORMAT;
    else if (strlen(value) >= capacity)
      result = H2_PAL_ERR_NO_SPACE;
  }
  h2_pal_result_t closed = namespace->close != NULL
                               ? namespace->close(namespace)
                               : H2_PAL_ERR_UNSUPPORTED;
  if (result == H2_PAL_OK) result = closed;
  if (result == H2_PAL_OK) memcpy(out, value, strlen(value) + 1u);
  h2_pal_mem_free(runtime->mem, value);
  return result;
}

h2_pal_result_t h2_alipay_tiga_read_imei(void* user, char* out,
                                         size_t capacity) {
  h2_runtime_t* runtime = user;
  if (runtime == NULL || out == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  h2_pal_modem_identity_t identity = {0};
  h2_pal_result_t result = h2_pal_modem_get_identity(runtime->modem, &identity);
  if (result != H2_PAL_OK) return result;
  const char* end = memchr(identity.imei, '\0', sizeof(identity.imei));
  if (end == NULL || end == identity.imei) return H2_PAL_ERR_UNAVAILABLE;
  size_t length = (size_t)(end - identity.imei);
  if (length >= capacity) return H2_PAL_ERR_NO_SPACE;
  memcpy(out, identity.imei, length + 1u);
  return H2_PAL_OK;
}
