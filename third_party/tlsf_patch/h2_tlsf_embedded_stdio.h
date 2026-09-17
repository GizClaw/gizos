#ifndef H2_TLSF_EMBEDDED_STDIO_H
#define H2_TLSF_EMBEDDED_STDIO_H

#include <stdio.h>

/* Firmware libraries cannot acquire a diagnostics backend through libc.
 * Keep upstream unchanged; the Host validates pool sizes before TLSF calls.
 * A function (rather than discarding macro arguments) preserves use tracking
 * and upstream format/type checking without emitting stdout references. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static inline int h2_tlsf_embedded_printf(const char *format, ...) {
  (void)format;
  return 0;
}

#define printf h2_tlsf_embedded_printf

#endif
