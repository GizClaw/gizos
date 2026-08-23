/*
 * Embedded-only fail-closed stdio shims for upstream Lua helper functions that
 * Firmwares never exposes. The build overlay renames those calls so the vendor
 * source remains unmodified and cannot acquire a newlib standard-stream ABI.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

struct _reent;

static FILE s_disabled_stdin;
_Thread_local int _tls_errno;
_Thread_local FILE *_tls_stdin = &s_disabled_stdin;

/*
 * The ESP32-S3 compiler's newlib compatibility ctype.h expands the ctype
 * macros through this legacy table, while ESP-IDF 6 links picolibc and does
 * not export the table. Lua's lexical rules are deliberately ASCII, so an
 * embedded-only ASCII table is both sufficient and locale independent.
 */
const char _ctype_[257] = {
    [0 + 1 ... 8 + 1] = _C,          ['\t' + 1 ... '\r' + 1] = _C | _S,
    [14 + 1 ... 31 + 1] = _C,        [' ' + 1] = _B | _S,
    ['!' + 1 ... '/' + 1] = _P,      ['0' + 1 ... '9' + 1] = _N | _X,
    [':' + 1 ... '@' + 1] = _P,      ['A' + 1 ... 'F' + 1] = _U | _X,
    ['G' + 1 ... 'Z' + 1] = _U,      ['[' + 1 ... '`' + 1] = _P,
    ['a' + 1 ... 'f' + 1] = _L | _X, ['g' + 1 ... 'z' + 1] = _L,
    ['{' + 1 ... '~' + 1] = _P,      [127 + 1] = _C,
};

int __srget_r(struct _reent *reent, FILE *stream) {
  (void)reent;
  (void)stream;
  return EOF;
}

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
