/*
 * Force-included into embedded builds of the vendored Lua. C libraries define
 * stdin, stdout and stderr in <stdio.h> through their reentrancy state (newlib
 * expands them to _impure_ptr fields), which would override command-line
 * renames. Include <stdio.h> first, then point the standard streams at the
 * fail-closed shims in lua_portability.c.
 */

#ifndef H2_LUA_EMBEDDED_STDIO_H
#define H2_LUA_EMBEDDED_STDIO_H

#include <stdio.h>

extern FILE *h2_lua_disabled_stdin;
extern FILE *h2_lua_disabled_stdout;
extern FILE *h2_lua_disabled_stderr;

#undef stdin
#undef stdout
#undef stderr
#define stdin h2_lua_disabled_stdin
#define stdout h2_lua_disabled_stdout
#define stderr h2_lua_disabled_stderr

#endif
