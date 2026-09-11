/*
 * Embedded-only fail-closed stdio shims for upstream Lua helper functions that
 * Firmwares never exposes. The build overlay renames those calls so the vendor
 * source remains unmodified and cannot acquire a newlib standard-stream ABI.
 */

#include <stdarg.h>
#include <stdio.h>

FILE *h2_lua_disabled_stdin;
FILE *h2_lua_disabled_stdout;
FILE *h2_lua_disabled_stderr;

FILE *h2_lua_disabled_fopen(const char *path, const char *mode) {
  (void)path;
  (void)mode;
  return NULL;
}

FILE *h2_lua_disabled_freopen(const char *path, const char *mode,
                              FILE *stream) {
  (void)path;
  (void)mode;
  (void)stream;
  return NULL;
}

size_t h2_lua_disabled_fread(void *buffer, size_t size, size_t count,
                             FILE *stream) {
  (void)buffer;
  (void)size;
  (void)count;
  (void)stream;
  return 0u;
}

size_t h2_lua_disabled_fwrite(const void *buffer, size_t size, size_t count,
                              FILE *stream) {
  (void)buffer;
  (void)size;
  (void)count;
  (void)stream;
  return 0u;
}

int h2_lua_disabled_fclose(FILE *stream) {
  (void)stream;
  return EOF;
}

int h2_lua_disabled_ferror(FILE *stream) {
  (void)stream;
  return 1;
}

int h2_lua_disabled_fprintf(FILE *stream, const char *format, ...) {
  va_list arguments;
  (void)stream;
  (void)format;
  va_start(arguments, format);
  va_end(arguments);
  return -1;
}

int h2_lua_disabled_fflush(FILE *stream) {
  (void)stream;
  return EOF;
}

int h2_lua_disabled_fputs(const char *text, FILE *stream) {
  (void)text;
  (void)stream;
  return EOF;
}

int h2_lua_disabled_fputc(int character, FILE *stream) {
  (void)character;
  (void)stream;
  return EOF;
}

int h2_lua_disabled_getc(FILE *stream) {
  (void)stream;
  return EOF;
}
