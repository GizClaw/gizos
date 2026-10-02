#include "h2/pal/core/h2_pal_errors.h"

#include <climits>
#include <cstdint>
#include <type_traits>

static_assert(std::is_same<h2_pal_result_t, int>::value,
              "PAL results must preserve the C int callback ABI");
static_assert(sizeof(h2_pal_result_t) * CHAR_BIT == 32,
              "PAL results require a signed 32-bit representation");
static_assert(static_cast<h2_pal_result_t>(-1000) == -1000,
              "library errors must not narrow to a positive status");
static_assert(static_cast<h2_pal_result_t>(INT32_MIN) == INT32_MIN,
              "PAL results must cover every signed 32-bit error");

static int legacy_callback(int result) { return result; }
static h2_pal_result_t result_callback(int result) { return result; }
static h2_pal_result_t (*const result_abi_callback)(int) = legacy_callback;
static int (*const legacy_abi_callback)(int) = result_callback;

int main() {
    return result_abi_callback(-1000) == -1000 &&
           legacy_abi_callback(INT32_MIN) == INT32_MIN ? 0 : 1;
}
