#include "h2_bk_platform_core.h"

#include <components/log.h>
#include <stdio.h>

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

/* Raw records use the SDK console owner, without Log decoration or filtering.
 * The caller supplies framing; no newlib stdout/FILE object is borrowed. */
static int bk_console_log_write(void* user, h2_pal_log_level_t level,
                                const char* scope, const char* message) {
  (void)user;
  (void)scope;
  if (message == NULL || level < H2_PAL_LOG_DEBUG || level > H2_PAL_LOG_ERROR)
    return H2_PAL_LOG_ERR_INVALID_ARG;
  return printf("%s", message) < 0 ? H2_PAL_ERR_IO : H2_PAL_LOG_OK;
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
