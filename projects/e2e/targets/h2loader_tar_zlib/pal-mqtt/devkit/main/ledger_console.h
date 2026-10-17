#ifndef H2_MQTT_ESP_LEDGER_CONSOLE_H
#define H2_MQTT_ESP_LEDGER_CONSOLE_H

#include "device_runner.h"
#include "result_console.h"
#include <stdio.h>
#include <string.h>

/* Borrow the console driver already installed by H2Loader. Each
 * record has one bounded write and a checked physical TX drain; no prefix retry
 * can make an incomplete first ledger eligible for persistent confirmation. */
typedef h2_esp_e2e_console_t h2_mqtt_esp_ledger_console_t;

static inline int h2_mqtt_esp_console_record(h2_mqtt_esp_ledger_console_t *console,
    const char *message) {
    return h2_esp_e2e_console_record(console,message);
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
    const char *board=runtime->board!=NULL?runtime->board:"devkit";
    char line[160];
    int length=snprintf(line,sizeof(line),"H2_PAL_MQTT_READY board=%s rc=0 confirm=pending provider_cleanup=0",board);
    if(length<0 || (size_t)length>=sizeof(line))return H2_PAL_ERR_TRUNCATED;
    rc=h2_mqtt_esp_console_record(console,line);
    if(rc!=H2_PAL_OK)return rc;
    int confirmed=confirm(user);
    length=snprintf(line,sizeof(line),"H2_PAL_MQTT_CONFIRMED board=%s rc=%d",board,confirmed);
    if(length<0 || (size_t)length>=sizeof(line))return H2_PAL_ERR_TRUNCATED;
    rc=h2_mqtt_esp_console_record(console,line);
    return confirmed!=H2_PAL_OK?confirmed:rc;
}

#endif
