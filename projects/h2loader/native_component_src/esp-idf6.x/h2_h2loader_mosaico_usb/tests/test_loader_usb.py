"""Generate a host fixture from the actual startup configuration functions."""
import argparse
from pathlib import Path


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args()
    source = Path(args.source).read_text()
    prelude = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_PAL_ERR_INVALID_STATE -2
#define H2_PAL_ERR_TIMEOUT -3
#define H2_PAL_ERR_IO -4
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 9
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define H2_LOADER_SERIAL_RX_BUFFER 8192
#define H2_LOADER_SERIAL_TX_BUFFER 8192
#define pdMS_TO_TICKS(x) (x)
typedef int h2_pal_result_t;
typedef int esp_err_t;
typedef struct {int rx_buffer_size, tx_buffer_size;} usb_serial_jtag_driver_config_t;
typedef struct {void* user; int (*read)(void*,void*,size_t,size_t*,uint32_t); int (*write)(void*,const void*,size_t,size_t*,uint32_t); int (*flush)(void);} h2_iostreamikcp_io_t;
static int s_console_initialized, s_physical_io_configured, s_loader_initialization_started;
static h2_iostreamikcp_io_t s_physical_io;
static int installs, fail_install;
static int usb_serial_jtag_is_driver_installed(void) {return 0;}
static int usb_serial_jtag_wait_tx_done(int t) {(void)t;return 0;}
static int usb_serial_jtag_driver_uninstall(void) {return 0;}
static int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t* c) {(void)c;installs++;return fail_install?ESP_ERR_TIMEOUT:0;}
static void usb_serial_jtag_vfs_use_driver(void) {}
'''
    code = prelude + function(source, "h2_pal_result_t h2_esp_h2loader_configure_physical_io(")
    code += function(source, "h2_pal_result_t h2_esp_h2loader_console_init(")
    code += r'''
#define H2_PAL_ERR_CLOSED -5
#define H2_PAL_ERR_WOULD_BLOCK -6
#define H2_LOADER_TRANSPORT_PHYSICAL_READ_SIZE 16
#define H2_LOADER_TRANSPORT_POLL_INTERVAL_MS 5
typedef struct { void* stream; h2_iostreamikcp_io_t physical_io; int filter; int pending_conv,replacement_pending,close_pending; } h2_esp_h2loader_command_transport_t;
static int updates_return, read_returns, deactivations;
static void h2_esp_h2loader_command_transport_deactivate(h2_esp_h2loader_command_transport_t* t) {t->stream=NULL;deactivations++;}
static void h2_iostreamikcp_filter_init(int* f) {*f=0;}
#define h2_iostreamikcp_filter_input(...) H2_PAL_OK
static int h2_iostreamikcp_update(void* s,uint32_t n) {(void)s;(void)n;return updates_return;}
static uint32_t transport_now_ms(void* u) {(void)u;return 0;}
static int read_fault(void* u,void* p,size_t n,size_t* out,uint32_t timeout) {(void)u;(void)p;(void)n;(void)timeout;*out=0;return read_returns;}
'''
    code += function(source, "static h2_pal_result_t transport_poll_physical(")
    code += r'''
static int callback(void* u,void* p,size_t n,size_t* o,uint32_t t) {(void)u;(void)p;(void)n;(void)o;(void)t;return 0;}
static int write_callback(void* u,const void* p,size_t n,size_t* o,uint32_t t) {(void)u;(void)p;(void)n;(void)o;(void)t;return 0;}
int main(void) {
  int context;
  h2_iostreamikcp_io_t io = {.user=&context,.read=callback,.write=write_callback};
  assert(h2_esp_h2loader_configure_physical_io(NULL)==H2_PAL_ERR_INVALID_ARG);
  h2_iostreamikcp_io_t bad={0};
  assert(h2_esp_h2loader_configure_physical_io(&bad)==H2_PAL_ERR_INVALID_ARG);
  assert(!s_physical_io_configured && !s_loader_initialization_started);
  assert(h2_esp_h2loader_configure_physical_io(&io)==H2_PAL_OK);
  io.user=NULL;io.read=NULL;
  assert(s_physical_io.user==&context && s_physical_io.read==callback);
  assert(h2_esp_h2loader_configure_physical_io(&io)==H2_PAL_ERR_INVALID_ARG);
  io.read=callback;
  assert(h2_esp_h2loader_configure_physical_io(&io)==H2_PAL_ERR_INVALID_STATE);
  assert(h2_esp_h2loader_console_init()==H2_PAL_OK && installs==0);
  assert(h2_esp_h2loader_console_init()==H2_PAL_OK && installs==0);
  s_console_initialized=0;s_physical_io_configured=0;s_loader_initialization_started=0;
  fail_install=1;
  assert(h2_esp_h2loader_console_init()==H2_PAL_ERR_TIMEOUT);
  assert(h2_esp_h2loader_configure_physical_io(&io)==H2_PAL_ERR_INVALID_STATE);
  fail_install=0;
  assert(h2_esp_h2loader_console_init()==H2_PAL_OK && installs==2);
  h2_esp_h2loader_command_transport_t transport={.stream=&context,.physical_io={.read=read_fault},.filter=3,.pending_conv=7,.replacement_pending=1,.close_pending=1};
  s_physical_io_configured=1;
  read_returns=H2_PAL_ERR_TIMEOUT;
  assert(transport_poll_physical(&transport,10)==H2_PAL_ERR_TIMEOUT && deactivations==0 && transport.stream);
  read_returns=H2_PAL_ERR_CLOSED;
  assert(transport_poll_physical(&transport,10)==H2_PAL_ERR_CLOSED && deactivations==1 && !transport.stream && !transport.filter && !transport.pending_conv && !transport.replacement_pending && !transport.close_pending);
  transport.stream=&context;read_returns=H2_PAL_OK;updates_return=H2_PAL_ERR_CLOSED;
  assert(transport_poll_physical(&transport,10)==H2_PAL_ERR_CLOSED && deactivations==2 && !transport.stream);
  s_physical_io_configured=0;transport.stream=&context;
  assert(transport_poll_physical(&transport,10)==H2_PAL_ERR_CLOSED && deactivations==2 && transport.stream);
  return 0;
}
'''
    Path(args.output).write_text(code)


if __name__ == "__main__":
    main()
