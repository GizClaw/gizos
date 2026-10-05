#include "device_runner.h"
#include "h2_pal_mqtt_fixture_config.h"
#include <stdio.h>
#include <string.h>
const char h2_pal_mqtt_device_runner_task_name[] = "pal-mqtt/e2e/runner";
static int hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}
static void encoded(const uint8_t *bytes, size_t length, char *out) {
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < length; ++i) {out[i * 2u] = digits[bytes[i] >> 4];out[i * 2u + 1u] = digits[bytes[i] & 15u];}
    out[length * 2u] = '\0';
}
static int pem(h2_runtime_t *runtime, const char *text, uint8_t **out, size_t *length) {
    *out = NULL; size_t size = strlen(text);
    if (size == 0u || size % 2u || size > 16384u) return H2_PAL_ERR_INVALID_ARG;
    *length = size / 2u; *out = h2_pal_mem_alloc(runtime->mem, *length);
    if (*out == NULL) return H2_PAL_ERR_NO_MEMORY;
    for (size_t i = 0u; i < size; i += 2u) {
        int high = hex(text[i]), low = hex(text[i + 1u]);
        if (high < 0 || low < 0) {h2_pal_mem_free(runtime->mem,*out);*out=NULL;return H2_PAL_ERR_INVALID_ARG;}
        (*out)[i / 2u] = (uint8_t)((high << 4) | low);
    }
    return H2_PAL_OK;
}
int h2_mqtt_device_prepare(h2_runtime_t *runtime) {
    if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_status_t current = {0};
    if (h2_pal_wifi_sta_get_status(runtime->wifi_sta,&current)==H2_PAL_OK &&
        current.state==H2_PAL_WIFI_STA_STATE_GOT_IP && current.ip_valid && current.ip.ip4!=0u) return H2_PAL_OK;
    h2_pal_wifi_sta_config_t wifi = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings,&wifi);
    if (rc==H2_PAL_OK) rc=h2_pal_wifi_sta_connect(runtime->wifi_sta,&wifi,20000u);
    memset(&wifi,0,sizeof(wifi));
    if (rc!=H2_PAL_OK) return rc;
    uint64_t start=0u;rc=h2_pal_time_get_monotonic_ms(runtime->time,&start);
    while(rc==H2_PAL_OK) {
        h2_pal_wifi_sta_status_t status={0};rc=h2_pal_wifi_sta_get_status(runtime->wifi_sta,&status);
        if(rc!=H2_PAL_OK)return rc;
        if(status.state==H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid && status.ip.ip4!=0u)return H2_PAL_OK;
        uint64_t now=0u;rc=h2_pal_time_get_monotonic_ms(runtime->time,&now);
        if(rc!=H2_PAL_OK)return rc;
        if(now-start>=20000u)return H2_PAL_ERR_TIMEOUT;
        rc=h2_pal_time_sleep_ms(runtime->time,100u);
    }
    return rc;
}
int h2_mqtt_device_run(h2_runtime_t *runtime, unsigned capacity,
    h2_mqtt_device_digest_fn hash, void *hash_user, h2_mqtt_device_result_t *out) {
    if(runtime==NULL || out==NULL || hash==NULL)return H2_PAL_ERR_INVALID_ARG;
    uint8_t *ca=NULL,*wrong=NULL;size_t ca_length=0u,wrong_length=0u;
    int rc=H2_PAL_ERR_INVALID_ARG;
    if(H2_PAL_MQTT_HOST[0]=='\0' || H2_PAL_MQTT_SESSION_PREFIX[0]=='\0' ||
        H2_PAL_MQTT_TCP_PORT==0u || H2_PAL_MQTT_TCP_PORT>UINT16_MAX ||
        H2_PAL_MQTT_TLS_PORT==0u || H2_PAL_MQTT_TLS_PORT>UINT16_MAX || H2_PAL_MQTT_FIXTURE_EPOCH_MS==0u)goto done;
    rc=pem(runtime,H2_PAL_MQTT_CA_PEM_HEX,&ca,&ca_length);
    if(rc==H2_PAL_OK)rc=pem(runtime,H2_PAL_MQTT_WRONG_CA_PEM_HEX,&wrong,&wrong_length);
    if(rc!=H2_PAL_OK)goto done;
    uint8_t nonce[8],digest[32];char nonce_hex[17];
    rc=h2_pal_crypto_random(runtime->crypto,nonce,sizeof(nonce));if(rc!=H2_PAL_OK)goto done;
    encoded(nonce,sizeof(nonce),nonce_hex);
    int count=snprintf(out->execution,sizeof(out->execution),"%s-%s",H2_PAL_MQTT_SESSION_PREFIX,nonce_hex);
    if(count<=0 || (size_t)count>=sizeof(out->execution)){rc=H2_PAL_ERR_INVALID_ARG;goto done;}
    rc=hash(hash_user,ca,ca_length,digest);if(rc!=H2_PAL_OK)goto done;
    encoded(digest,sizeof(digest),out->ca_sha256);
    out->epoch_ms=H2_PAL_MQTT_FIXTURE_EPOCH_MS;
    rc=h2_pal_time_set_wall_ms(runtime->time,out->epoch_ms);if(rc!=H2_PAL_OK)goto done;
    h2_pal_firmware_info_t image={0};rc=h2_pal_firmware_info_get_current(runtime->firmware_info,&image);
    if(rc!=H2_PAL_OK)goto done;
    char boot_line[384];
    count=snprintf(boot_line,sizeof(boot_line),"H2_PAL_MQTT_BOOT id=%s version=%s epoch_ms=%llu ca_sha256=%s",
        out->execution,image.version,(unsigned long long)out->epoch_ms,out->ca_sha256);
    if(count<=0 || (size_t)count>=sizeof(boot_line)){rc=H2_PAL_ERR_NO_SPACE;goto done;}
    if(h2_pal_log_write(runtime->log,H2_PAL_LOG_INFO,"pal-mqtt",boot_line)!=H2_PAL_LOG_OK){rc=H2_PAL_ERR_IO;goto done;}
    h2_pal_net_tls_config_t trusted={.server_name="localhost",.root_ca_pem=ca,.root_ca_pem_len=ca_length,.verify=H2_PAL_NET_TLS_VERIFY_REQUIRED};
    h2_pal_net_tls_config_t untrusted=trusted,wrong_name=trusted;
    untrusted.root_ca_pem=wrong;untrusted.root_ca_pem_len=wrong_length;wrong_name.server_name="wrong-name.invalid";
    h2_pal_mqtt_e2e_config_t config={.runtime=runtime,.host=H2_PAL_MQTT_HOST,.tcp_port=(uint16_t)H2_PAL_MQTT_TCP_PORT,
        .tls_port=(uint16_t)H2_PAL_MQTT_TLS_PORT,.session=out->execution,.topic_prefix="h2/mqtt/e2e",
        .trusted_tls=&trusted,.untrusted_tls=&untrusted,.wrong_name_tls=&wrong_name,
        .timeout_ms=15000u,.negative_connect_timeout_ms=2000u,.qos_publish_capacity=capacity};
    rc=h2_pal_mqtt_e2e_run(&config,&out->suite);
done:
    h2_pal_mem_free(runtime->mem,wrong);h2_pal_mem_free(runtime->mem,ca);
    out->rc=rc;return rc;
}
static int append_values(char *line,size_t capacity,size_t *offset,const size_t values[10]) {
    for(unsigned i=0u;i<10u;++i){
        int count=snprintf(line+*offset,capacity-*offset,"%s%zu",i?",":"",values[i]);
        if(count<0 || (size_t)count>=capacity-*offset)return H2_PAL_ERR_NO_SPACE;
        *offset+=(size_t)count;
    }
    return H2_PAL_OK;
}
int h2_mqtt_device_replay(h2_runtime_t *runtime,const h2_mqtt_device_result_t *result) {
    if(runtime==NULL || result==NULL)return H2_PAL_ERR_INVALID_ARG;
    h2_pal_firmware_info_t image={0};
    int rc=h2_pal_firmware_info_get_current(runtime->firmware_info,&image);
    if(rc!=H2_PAL_OK)return rc;
    char line[768];
    int count=snprintf(line,sizeof(line),"H2_PAL_MQTT_RUN id=%s version=%s epoch_ms=%llu ca_sha256=%s",
        result->execution,image.version,(unsigned long long)result->epoch_ms,result->ca_sha256);
    if(count<0 || (size_t)count>=sizeof(line))return H2_PAL_ERR_NO_SPACE;
    rc=h2_pal_log_write(runtime->log,H2_PAL_LOG_INFO,"pal-mqtt",line);
    if(rc!=H2_PAL_LOG_OK)return rc;
    for(unsigned i=0u;i<H2_PAL_MQTT_E2E_CASE_COUNT;++i){
        const h2_pal_mqtt_e2e_case_result_t *row=&result->suite.cases[i];
        if(row->id==NULL)return H2_PAL_ERR_INVALID_STATE;
        count=snprintf(line,sizeof(line),"H2_PAL_MQTT_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"connected\":%u,\"received\":%u,\"disconnected\":%u}",
            row->id,row->passed?"PASS":row->blocked?"BLOCKED":"FAIL",row->detail,row->line,(unsigned long long)row->elapsed_ms,row->connected,row->received,row->disconnected);
        if(count<0 || (size_t)count>=sizeof(line))return H2_PAL_ERR_NO_SPACE;
        rc=h2_pal_log_write(runtime->log,H2_PAL_LOG_INFO,"pal-mqtt",line);
        if(rc!=H2_PAL_LOG_OK)return rc;
        rc=h2_pal_time_sleep_ms(runtime->time,40u);
        if(rc!=H2_PAL_OK)return rc;
    }
    count=snprintf(line,sizeof(line),"H2_PAL_MQTT_SUMMARY {\"selected\":%u,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"cleanup\":%d,\"rc\":%d,\"before\":[",
        result->suite.selected,result->suite.passed,result->suite.failed,result->suite.blocked,result->cleanup,result->rc);
    if(count<0 || (size_t)count>=sizeof(line))return H2_PAL_ERR_NO_SPACE;
    size_t offset=(size_t)count;
    if(append_values(line,sizeof(line),&offset,result->before)!=H2_PAL_OK)return H2_PAL_ERR_NO_SPACE;
    count=snprintf(line+offset,sizeof(line)-offset,"],\"after\":[");
    if(count<0 || (size_t)count>=sizeof(line)-offset)return H2_PAL_ERR_NO_SPACE;
    offset+=(size_t)count;
    if(append_values(line,sizeof(line),&offset,result->after)!=H2_PAL_OK)return H2_PAL_ERR_NO_SPACE;
    count=snprintf(line+offset,sizeof(line)-offset,"]}");
    if(count<0 || (size_t)count>=sizeof(line)-offset)return H2_PAL_ERR_NO_SPACE;
    return h2_pal_log_write(runtime->log,H2_PAL_LOG_INFO,"pal-mqtt",line);
}
