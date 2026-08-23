#include "h2_wolfssl_internal.h"

void *h2_wolfssl_alloc(size_t len) {
    extern h2_pal_mem_api_t h2_wolfssl_mem_api;
    return h2_pal_mem_alloc(&h2_wolfssl_mem_api, len);
}

void *h2_wolfssl_realloc(void *ptr, size_t len) {
    extern h2_pal_mem_api_t h2_wolfssl_mem_api;
    return h2_pal_mem_realloc(&h2_wolfssl_mem_api, ptr, len);
}

void h2_wolfssl_free(void *ptr) {
    extern h2_pal_mem_api_t h2_wolfssl_mem_api;
    h2_pal_mem_free(&h2_wolfssl_mem_api, ptr);
}
