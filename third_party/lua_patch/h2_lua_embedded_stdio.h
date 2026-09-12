/*
 * Force-included into embedded builds of the vendored Lua. C libraries may
 * define the standard streams and stdio functions such as getc() as macros
 * that expand through their reentrancy state (newlib uses _impure_ptr), which
 * would override command-line renames. Include <stdio.h> first, then route
 * every stdio name Lua uses to the fail-closed shims in lua_portability.c.
 */

#ifndef H2_LUA_EMBEDDED_STDIO_H
#define H2_LUA_EMBEDDED_STDIO_H

#include <stddef.h>
#include <stdio.h>

extern FILE *h2_lua_disabled_stdin;
extern FILE *h2_lua_disabled_stdout;
extern FILE *h2_lua_disabled_stderr;

FILE *h2_lua_disabled_fopen(const char *path, const char *mode);
FILE *h2_lua_disabled_freopen(const char *path, const char *mode, FILE *stream);
size_t h2_lua_disabled_fread(void *buffer, size_t size, size_t count,
                             FILE *stream);
size_t h2_lua_disabled_fwrite(const void *buffer, size_t size, size_t count,
                              FILE *stream);
int h2_lua_disabled_fclose(FILE *stream);
int h2_lua_disabled_ferror(FILE *stream);
int h2_lua_disabled_fprintf(FILE *stream, const char *format, ...);
int h2_lua_disabled_fflush(FILE *stream);
int h2_lua_disabled_fputs(const char *text, FILE *stream);
int h2_lua_disabled_fputc(int character, FILE *stream);
int h2_lua_disabled_getc(FILE *stream);

#undef stdin
#undef stdout
#undef stderr
#undef fopen
#undef freopen
#undef fread
#undef fwrite
#undef fclose
#undef ferror
#undef fprintf
#undef fflush
#undef fputs
#undef fputc
#undef getc

#define stdin h2_lua_disabled_stdin
#define stdout h2_lua_disabled_stdout
#define stderr h2_lua_disabled_stderr
#define fopen h2_lua_disabled_fopen
#define freopen h2_lua_disabled_freopen
#define fread h2_lua_disabled_fread
#define fwrite h2_lua_disabled_fwrite
#define fclose h2_lua_disabled_fclose
#define ferror h2_lua_disabled_ferror
#define fprintf h2_lua_disabled_fprintf
#define fflush h2_lua_disabled_fflush
#define fputs h2_lua_disabled_fputs
#define fputc h2_lua_disabled_fputc
#define getc h2_lua_disabled_getc

#endif
