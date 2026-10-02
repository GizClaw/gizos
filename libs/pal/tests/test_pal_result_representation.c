#include "h2/pal/core/h2_pal_errors.h"

#include <stdint.h>

/* BK's ARM ABI uses small enums. Library extensions, such as GizClaw's
 * remote error (-1000), must retain their signed value under that ABI. */
_Static_assert(sizeof(h2_pal_result_t) == sizeof(int32_t),
               "PAL results require a signed 32-bit representation");
_Static_assert((h2_pal_result_t)-1000 == -1000,
               "library errors must not narrow to a positive status");
_Static_assert((h2_pal_result_t)INT32_MIN == INT32_MIN,
               "PAL result representation must cover signed 32-bit errors");

static int legacy_callback(void) { return -1000; }
static h2_pal_result_t result_callback(void) { return -1000; }
/* These assignments must compile on newlib too, whose int32_t is long. */
static h2_pal_result_t (*const result_abi_callback)(void) = legacy_callback;
static int (*const legacy_abi_callback)(void) = result_callback;

int main(void) {
  return result_abi_callback() == -1000 && legacy_abi_callback() == -1000 ? 0 : 1;
}
