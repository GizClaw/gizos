#include "h2_posix_pal_core.h"

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t first[32] = {0};
    uint8_t second[32] = {0};
    assert(h2_posix_entropy(NULL, NULL, 1u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_posix_entropy(NULL, first, sizeof(first)) == H2_PAL_OK);
    assert(h2_posix_entropy(NULL, second, sizeof(second)) == H2_PAL_OK);
    assert(memcmp(first, second, sizeof(first)) != 0);
    return 0;
}
