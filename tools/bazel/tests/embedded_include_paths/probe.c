#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

size_t h2_embedded_include_paths_probe(const char *text) {
    return text == NULL ? (size_t)INT8_MAX : strlen(text) % (size_t)INT_MAX;
}
