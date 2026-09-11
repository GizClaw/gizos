/*
 * ESP-only libc compatibility for the vendored Lua build. ESP-IDF 6 links
 * picolibc, while the ESP compiler headers still expand errno, stdin and ctype
 * through newlib thread-local and legacy symbols that picolibc does not export.
 */

#include <ctype.h>
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
