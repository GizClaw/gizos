#include "h2_encoding.h"

int main() {
    char text[4];
    size_t n = 0;
    const uint8_t bytes[] = {'f', 'o'};
    h2_encoding_t star;
    if (h2_encoding_init(&star, H2_ENCODING_KIND_BASE64, h2_encoding_base64_std.alphabet, '*',
                         '\0') != H2_ENCODING_OK) {
        return 1;
    }
    return h2_encoding_encode(&star, bytes, sizeof(bytes), text, sizeof(text), &n) ==
                       H2_ENCODING_OK &&
                   n == 4 && text[3] == '*'
               ? 0
               : 1;
}
