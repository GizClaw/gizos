#include "h2/pal/core/h2_pal_errors.h"

#include <limits.h>
#include <stdint.h>

/* Array bounds also check this contract in default MSVC C mode. */
typedef char result_width[sizeof(h2_pal_result_t) * CHAR_BIT == 32 ? 1 : -1];
typedef char extended_error[(h2_pal_result_t)-1000 == -1000 ? 1 : -1];
typedef char minimum_error[(h2_pal_result_t)INT32_MIN == INT32_MIN ? 1 : -1];
typedef char maximum_result[(h2_pal_result_t)INT32_MAX == INT32_MAX ? 1 : -1];

#ifndef _MSC_VER
/* Prove that the GNU-style test actually runs with compressed enums. */
enum short_enum_control { SHORT_ENUM_NEGATIVE = -1, SHORT_ENUM_POSITIVE = 1 };
typedef char compressed_enum[sizeof(enum short_enum_control) < sizeof(int) ? 1 : -1];
#endif

static int legacy_callback(int result) { return result; }
static h2_pal_result_t result_callback(int result) { return result; }

/* int32_t can be long on newlib; these must retain the C int callback ABI. */
static h2_pal_result_t (*const result_abi_callback)(int) = legacy_callback;
static int (*const legacy_abi_callback)(int) = result_callback;

int main(void) {
    const int results[] = {-1000, INT32_MIN, INT32_MAX, H2_PAL_ERR_BUSY};
    unsigned int index;
    for (index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
        if (result_abi_callback(results[index]) != results[index] ||
            legacy_abi_callback(results[index]) != results[index]) {
            return 1;
        }
    }
    return 0;
}
