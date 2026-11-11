#include "h2_bk_platform_core.h"
#include "h2_bk_console_tx_internal.h"

#include <components/log.h>
#include <stdio.h>
#include <string.h>

static int bk_platform_log_write(void* user, h2_pal_log_level_t level,
                                 const char* scope, const char* message) {
  (void)user;

  if (message == NULL) {
    return H2_PAL_LOG_ERR_INVALID_ARG;
  }

  char tag[32];
  snprintf(tag, sizeof(tag), "%s",
           scope != NULL && scope[0] != '\0' ? scope : "h2");
  switch (level) {
    case H2_PAL_LOG_DEBUG:
      BK_LOGD(tag, "%s\r\n", message);
      break;
    case H2_PAL_LOG_INFO:
      BK_LOGI(tag, "%s\r\n", message);
      break;
    case H2_PAL_LOG_WARN:
      BK_LOGW(tag, "%s\r\n", message);
      break;
    case H2_PAL_LOG_ERROR:
      BK_LOGE(tag, "%s\r\n", message);
      break;
    default:
      return H2_PAL_LOG_ERR_INVALID_ARG;
  }
  return H2_PAL_LOG_OK;
}

/* Mandatory records bypass the SDK's lossy async printf pool. One bounded
 * write retains TX arbitration across the complete caller-framed record and
 * reports partial transfer/drain failure. No RX session is acquired. */
static int bk_console_log_write(void* user, h2_pal_log_level_t level,
                                const char* scope, const char* message) {
  (void)user;
  (void)scope;
  if (message == NULL || level < H2_PAL_LOG_DEBUG || level > H2_PAL_LOG_ERROR)
    return H2_PAL_LOG_ERR_INVALID_ARG;
  size_t len = strlen(message), written = 0u;
  h2_pal_result_t rc = h2_bk_console_write(message, len, &written, 5000u);
  return rc == H2_PAL_OK && written != len ? H2_PAL_ERR_IO : rc;
}

const h2_pal_log_api_t* h2_bk_platform_console_log_api(void) {
  static const h2_pal_log_vtable_t vtable = {.write = bk_console_log_write};
  static const h2_pal_log_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_log_api_t* h2_bk_platform_log_api(void) {
  static const h2_pal_log_vtable_t vtable = {
      .write = bk_platform_log_write,
  };
  static const h2_pal_log_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
