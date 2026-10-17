#ifndef H2_STORAGE_ESP_LEDGER_CONSOLE_H
#define H2_STORAGE_ESP_LEDGER_CONSOLE_H

#include "device_runner.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include "h2/pal/hal/h2_pal_usb_jtag_io_stream.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow the already configured console. Each record uses one PAL write and
 * a checked TX drain; short writes are never retried as a successful ledger. */
typedef struct h2_storage_esp_console {
    const h2_pal_uart_io_stream_api_t *uart;
    const h2_pal_usb_jtag_io_stream_api_t *usb;
    uint32_t timeout_ms;
    int error;
} h2_storage_esp_console_t;

static inline int h2_storage_esp_console_record(void *user,const char *record,size_t length) {
    h2_storage_esp_console_t *console=(h2_storage_esp_console_t *)user;
    if(console==NULL)return H2_PAL_ERR_INVALID_ARG;
    if(console->error)return console->error;
    if(record==NULL || length==0u || length>=H2_STORAGE_DEVICE_RECORD_MAX ||
        console->timeout_ms==0u || (console->uart==NULL)==(console->usb==NULL))
        return console->error=H2_PAL_ERR_INVALID_ARG;
    size_t written=0u;
    int rc=console->uart!=NULL
        ? h2_pal_uart_io_stream_write(console->uart,record,length,&written,console->timeout_ms)
        : h2_pal_usb_jtag_io_stream_write(console->usb,record,length,&written,console->timeout_ms);
    if(!rc && written!=length)rc=H2_PAL_ERR_IO;
    if(!rc)rc=console->uart!=NULL
        ? h2_pal_uart_io_stream_flush(console->uart)
        : h2_pal_usb_jtag_io_stream_flush(console->usb);
    if(rc)console->error=rc;
    return rc;
}

/* A complete replay must drain before confirmation. READY describes the
 * actual confirm result; a later output error cannot undo committed metadata. */
static inline int h2_storage_esp_console_complete(h2_storage_esp_console_t *console,
    h2_runtime_t *runtime,int suite_rc,int (*confirm)(void *),void *user) {
    if(console==NULL || runtime==NULL || confirm==NULL)return H2_PAL_ERR_INVALID_ARG;
    if(console->error)return console->error;
    if(suite_rc)return suite_rc;
    int rc=h2_storage_device_replay_checked(runtime);
    if(rc)return rc;
    int confirmed=confirm(user);
    char line[96];
    int length=snprintf(line,sizeof(line),"H2_STORAGE_READY rc=0 confirm=%d\n",confirmed);
    if(length<0 || (size_t)length>=sizeof(line))return H2_PAL_ERR_TRUNCATED;
    rc=h2_storage_esp_console_record(console,line,(size_t)length);
    return confirmed?confirmed:rc;
}

#ifdef __cplusplus
}
#endif
#endif
