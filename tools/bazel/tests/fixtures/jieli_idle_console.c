#include <assert.h>
#include "h2_atomic.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_TIMEOUT -1
#define H2_PAL_ERR_WOULD_BLOCK -2
#define H2_PAL_ERR_CLOSED -3
#define H2_PAL_ERR_INVALID_ARG -4
#define H2_LOADER_APP_CLIENT_SESSION_RESET -5
#define H2_LOADER_APP_CLIENT_SESSION_CLOSED -6
#define H2_LOADER_APP_CLIENT_POLL_MS 50u
#define H2_PHYSICAL_POLL_MS 10u
#define H2_PHYSICAL_READ_SIZE 64u
typedef int h2_pal_result_t;
typedef int (*read_fn)(void *, void *, size_t, size_t *, uint32_t);
typedef struct { read_fn read; } h2_command_io_vtable_t;
typedef struct { void *user; const h2_command_io_vtable_t *vtable; } h2_command_io_api_t;
typedef struct { void *user; read_fn read; } physical_io_t;
typedef struct {
 void *stream; physical_io_t physical_io; int filter;
 unsigned pending_conv; int close_pending, replacement_pending;
} h2_jieli_app_transport_t;
typedef struct { h2_jieli_app_transport_t transport; h2_command_io_api_t io; int started; } h2_jieli_app_console_t;
typedef struct { h2_command_io_api_t io; } h2_loader_command_t;
typedef h2_loader_command_t h2_loader_command_config_t;
typedef struct {
 void *client, *read_user; int (*read_byte)(void *, uint32_t);
 void *write_user; int (*write)(void *, const char *, size_t);
 h2_atomic_bool_t stop_requested; int session_reset, session_closed; h2_loader_command_t command;
} h2_loader_app_client_return_console_t;
static unsigned physical_polls, command_polls, resets, bytes_read;
static int on_frame;
static h2_pal_result_t console_read(void *, void *, size_t, size_t *, uint32_t);
static const h2_command_io_vtable_t s_console_io_vtable = {.read = console_read};
static h2_loader_command_config_t command_config(void *client, h2_command_io_api_t io) {
 assert(client); return (h2_loader_command_config_t){io};
}
static int h2_loader_command_init(h2_loader_command_t *command, const h2_loader_command_config_t *config) {
 *command = *config; ++resets; return H2_PAL_OK;
}
static int h2_loader_command_poll(h2_loader_command_t *command, uint32_t timeout) {
 h2_loader_app_client_return_console_t *console = command->io.user;
 uint8_t byte = 0; size_t count = 0; ++command_polls;
 assert(command_polls < 10);
 int rc = command->io.vtable->read(command->io.user, &byte, 1, &count, timeout);
 if (count) { assert(byte == 's'); ++bytes_read; h2_atomic_store(&console->stop_requested, true); }
 return rc;
}
static uint32_t timer_get_ms(void) { return 1; }
static int h2_iostreamikcp_update(void *stream, uint32_t now) { (void)stream; (void)now; return 0; }
static int physical_read(void *user, void *buffer, size_t len, size_t *count, uint32_t timeout) {
 (void)user; assert(len && timeout == 50); *count = 0;
 if (++physical_polls <= 3) return H2_PAL_ERR_TIMEOUT;
 *(uint8_t *)buffer = 1; *count = 1; return 0;
}
static int h2_iostreamikcp_filter_input(int *filter, const void *buffer, size_t count, int callback, void *user) {
 (void)filter; (void)buffer; (void)callback;
 if (count) ((h2_jieli_app_transport_t *)user)->pending_conv = 7;
 return 0;
}
static void deactivate_current(h2_jieli_app_transport_t *transport) {
 transport->stream = NULL; transport->close_pending = 0;
}
static int activate_pending(h2_jieli_app_transport_t *transport) {
 assert(transport->pending_conv == 7); transport->pending_conv = 0; transport->stream = transport; return 0;
}
static int stream_read(void *user, void *buffer, size_t len, size_t *count, uint32_t timeout) {
 (void)user; (void)timeout; assert(len == 1); *(char *)buffer = 's'; *count = 1; return 0;
}
static int output(void *user, const char *bytes, size_t len) { (void)user; (void)bytes; (void)len; return 0; }
static int closed_read(void *user, uint32_t timeout) { (void)user; (void)timeout; return H2_LOADER_APP_CLIENT_SESSION_CLOSED; }
/* FUNCTIONS */
int main(void) {
 static const h2_command_io_vtable_t stream_vtable = {.read = stream_read};
 h2_jieli_app_console_t app = {.transport.physical_io.read = physical_read,
     .io.vtable = &stream_vtable, .started = 1};
 h2_loader_app_client_return_console_t console = {.client = &app, .read_user = &app,
     .read_byte = app_read_byte, .write = output};
 assert(h2_atomic_init(&console.stop_requested, false) == H2_ATOMIC_OK);
 assert(run_return_console(&console) == 0);
 assert(physical_polls == 4 && command_polls == 5 && bytes_read == 1);
 assert(resets == 2 && !console.session_closed && app.started);
 /* The distinct closed sentinel still terminates the shared loop. */
 command_polls = 0; console.read_byte = closed_read; h2_atomic_store(&console.stop_requested, false);
 assert(run_return_console(&console) == 0);
 assert(command_polls == 1 && console.session_closed);
 /* A stop request exits without another transport poll. */
 command_polls = 0; h2_atomic_store(&console.stop_requested, true);
 assert(run_return_console(&console) == 0 && command_polls == 0);
 h2_atomic_destroy(&console.stop_requested);
 return 0;
}
