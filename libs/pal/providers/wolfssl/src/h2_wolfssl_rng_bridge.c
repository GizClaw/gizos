#include "h2_wolfssl_internal.h"

#include <wolfssl/wolfcrypt/error-crypt.h>

int h2_wolfssl_strcasecmp(const char *first, const char *second) {
    unsigned char first_char;
    unsigned char second_char;
    do {
        first_char = (unsigned char)*first++;
        second_char = (unsigned char)*second++;
        if (first_char >= (unsigned char)'A' &&
            first_char <= (unsigned char)'Z') {
            first_char = (unsigned char)(
                first_char + ((unsigned char)'a' - (unsigned char)'A'));
        }
        if (second_char >= (unsigned char)'A' &&
            second_char <= (unsigned char)'Z') {
            second_char = (unsigned char)(
                second_char + ((unsigned char)'a' - (unsigned char)'A'));
        }
    } while (first_char != 0u && first_char == second_char);
    return (int)first_char - (int)second_char;
}

int h2_wolfssl_generate_seed(unsigned char *out, unsigned int len) {
    if (h2_wolfssl_entropy_fill(out, (size_t)len) == H2_PAL_OK) {
        return 0;
    }
    if (out != NULL) {
        volatile unsigned char *bytes = out;
        unsigned int index;
        for (index = 0u; index < len; ++index) {
            bytes[index] = 0u;
        }
    }
    return RNG_FAILURE_E;
}
