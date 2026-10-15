#include <stdio.h>

/* The SDK's mbedtls_printf hook; only the OS logging sink is substituted. */
#define BK_LOGD(tag, ...) ((void)(tag), printf(__VA_ARGS__))
