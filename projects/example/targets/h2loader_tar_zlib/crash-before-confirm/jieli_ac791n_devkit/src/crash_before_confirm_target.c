#include "h2_crash_before_confirm.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"

#include <stdarg.h>
#include <stdio.h>

extern void cpu_assert_debug(void);

static void emit(const char *format, ...) {
  char line[160];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(line, (size_t)length, 100u);
  }
}

/* Take the SDK ASSERT failure path: the board hook records the retained
 * crash, then the CPU resets with the unconfirmed App bank still selected. */
static void crash_now(void *user) {
  (void)user;
  cpu_assert_debug();
}

/* The color-bar launcher runs this after the UART and BLE App command
 * services are live and before it confirms the trial image. */
int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  int result = h2_jieli_ac791n_devkit_runtime_config(&config);
  if (result == H2_PAL_OK) result = h2_runtime_init(&config, &runtime);
  if (result != H2_PAL_OK) {
    emit("H2_CRASH_BEFORE_CONFIRM_FAIL step=runtime code=%d\r\n", result);
    return result;
  }
  emit("H2_CRASH_BEFORE_CONFIRM_READY action=crash\r\n");
  const h2_crash_before_confirm_config_t crash_config = {.crash = crash_now};
  (void)h2_crash_before_confirm_run(runtime, &crash_config);
  cpu_assert_debug();
  return H2_PAL_ERR_INVALID_STATE;
}
