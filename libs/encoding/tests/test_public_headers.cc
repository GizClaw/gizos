#include "h2_encoding.h"

int main() {
    char text[4];
    size_t n = 0;
    const uint8_t bytes[] = {'f', 'o'};
    return h2_encoding_encode(&h2_encoding_base64_std, bytes, sizeof(bytes), text,
                              sizeof(text), &n) == H2_ENCODING_OK &&
                   n == 4 && text[3] == '='
               ? 0
               : 1;
}
