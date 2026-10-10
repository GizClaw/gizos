#ifndef H2_MQTT_ESP_LEDGER_CONSOLE_H
#define H2_MQTT_ESP_LEDGER_CONSOLE_H

#include "device_runner.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include <stdio.h>
#include <string.h>

/* This target uses the board diagnostic CDC0 sink initialized at startup. Each
 * record has one bounded write and a checked physical TX drain; no prefix retry
 * can make an incomplete first ledger eligible for persistent confirmation. */
typedef struct h2_mqtt_esp_ledger_console {
    const h2_pal_uart_io_stream_api_t *uart;
    uint32_t timeout_ms;
    int error;
} h2_mqtt_esp_ledger_console_t;

static inline int h2_mqtt_esp_console_record(h2_mqtt_esp_ledger_console_t *console,
    const char *message) {
    if(console->error!=H2_PAL_OK)return console->error;
    if(message==NULL || console->timeout_ms==0u)return console->error=H2_PAL_ERR_INVALID_ARG;
    size_t length=strlen(message);
    char record[1024];
    if(length==0u || length>sizeof(record)-2u || strchr(message,'\r') || strchr(message,'\n'))
        return console->error=H2_PAL_ERR_INVALID_ARG;
    memcpy(record,message,length);
    record[length++]='\r';record[length++]='\n';
    size_t written=0u;
    int rc=h2_pal_uart_io_stream_write(console->uart,record,length,&written,console->timeout_ms);
    if(rc==H2_PAL_OK && written!=length)rc=H2_PAL_ERR_IO;
    if(rc==H2_PAL_OK)rc=h2_pal_uart_io_stream_flush(console->uart);
    if(rc!=H2_PAL_OK)console->error=rc;
    return rc;
}

static inline int h2_mqtt_esp_console_complete(h2_mqtt_esp_ledger_console_t *console,
    h2_runtime_t *runtime,const h2_mqtt_device_result_t *result,int provider_cleanup,
    int (*confirm)(void *),void *user) {
    int rc=h2_mqtt_device_replay(runtime,result);
    if(rc!=H2_PAL_OK)return rc;
    if(result->rc!=H2_PAL_OK)return result->rc;
    if(result->cleanup!=H2_PAL_OK)return result->cleanup;
    if(provider_cleanup!=H2_PAL_OK)return provider_cleanup;
    if(confirm==NULL)return H2_PAL_ERR_INVALID_ARG;
    rc=h2_mqtt_esp_console_record(console,
        "H2_PAL_MQTT_READY board=esp_mosaico rc=0 confirm=pending provider_cleanup=0");
    if(rc!=H2_PAL_OK)return rc;
    int confirmed=confirm(user);
    char line[96];
    (void)snprintf(line,sizeof(line),"H2_PAL_MQTT_CONFIRMED board=esp_mosaico rc=%d",confirmed);
    rc=h2_mqtt_esp_console_record(console,line);
    return confirmed!=H2_PAL_OK?confirmed:rc;
}

#endif
