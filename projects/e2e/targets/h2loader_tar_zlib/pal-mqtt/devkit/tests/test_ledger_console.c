#include "ledger_console.h"
#include "projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/ap/ledger_console.h"
#include <assert.h>

static const char *const case_ids[]={
#define H2_PAL_MQTT_CASE(symbol,id) id,
#include "h2_pal_mqtt_cases.inc"
#undef H2_PAL_MQTT_CASE
};

typedef struct observed {
    FILE *wire;
    unsigned writes,flushes,confirms,fail_write,short_write,fail_flush;
    int firmware_error,confirm_error;
} observed_t;
static int emit_ledger,emit_bk;
static const char execution_id[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb";

static int write_record(void *user,const void *bytes,size_t length,size_t *written,uint32_t timeout) {
    observed_t *state=user;
    ++state->writes;
    assert(timeout==5000u);
    if(state->writes==state->fail_write){*written=0u;return H2_PAL_ERR_TIMEOUT;}
    if(state->writes==state->short_write)length=3u;
    *written=fwrite(bytes,1u,length,state->wire);
    return *written==length?H2_PAL_OK:H2_PAL_ERR_IO;
}
static int flush_record(void *user) {
    observed_t *state=user;
    ++state->flushes;
    if(state->flushes==state->fail_flush)return H2_PAL_ERR_TIMEOUT;
    return fflush(state->wire)==0?H2_PAL_OK:H2_PAL_ERR_IO;
}
static int log_record(void *user,h2_pal_log_level_t level,const char *scope,const char *message) {
    (void)level;assert(strcmp(scope,"pal-mqtt")==0);
    if(emit_bk)return h2_mqtt_bk_console_record(user,message);
    return h2_mqtt_esp_console_record(user,message);
}
static int write_uart(void *user,const void *bytes,size_t length,size_t *written,uint32_t timeout) {
    int rc=write_record(user,bytes,length,written,timeout);
    return rc==H2_PAL_OK?flush_record(user):rc;
}
static int firmware(void *user,h2_pal_firmware_info_t *out) {
    observed_t *state=user;
    if(state->firmware_error)return state->firmware_error;
    strcpy(out->version,"test-version");return H2_PAL_OK;
}
static int sleep_ms(void *user,uint32_t duration) {(void)user;assert(duration==40u);return H2_PAL_OK;}
static int confirm(void *user) {
    observed_t *state=user;
    ++state->confirms;
    /* Read the bytes delivered by the actual common replay function and the
     * target's real write/flush policy before allowing persistent confirmation. */
    char bytes[16384];
    long end=ftell(state->wire);assert(end>0);
    rewind(state->wire);
    size_t length=fread(bytes,1u,sizeof(bytes)-1u,state->wire);bytes[length]='\0';
    assert(strstr(bytes,execution_id)!=NULL && strstr(bytes,"version=test-version")!=NULL);
    unsigned count=0u;
    for(const char *row=bytes;(row=strstr(row,"H2_PAL_MQTT_CASE "))!=NULL;row+=18u)++count;
    assert(count==36u && strstr(bytes,"H2_PAL_MQTT_SUMMARY {\"selected\":36,\"passed\":36")!=NULL);
    const char *ready=emit_bk?"H2_PAL_MQTT_READY board=bk7258 rc=0 confirm=pending\r\n":
        "H2_PAL_MQTT_READY board=devkit rc=0 confirm=pending provider_cleanup=0\r\n";
    size_t ready_length=strlen(ready);
    assert(length>=ready_length && memcmp(bytes+length-ready_length,ready,ready_length)==0);
    assert(state->flushes==39u+(unsigned)emit_ledger);
    assert(fseek(state->wire,end,SEEK_SET)==0);
    return state->confirm_error;
}

static int execute(observed_t *state,int suite_error) {
    state->wire=tmpfile();assert(state->wire!=NULL);
    const h2_pal_usb_jtag_io_stream_vtable_t usb_vtable={.write=write_record,.flush=flush_record};
    const h2_pal_usb_jtag_io_stream_api_t usb={.user=state,.vtable=&usb_vtable};
    h2_mqtt_esp_ledger_console_t console={.usb=&usb,.timeout_ms=5000u};
    const h2_pal_uart_io_stream_vtable_t uart_vtable={.write=write_uart};
    const h2_pal_uart_io_stream_api_t uart={.user=state,.vtable=&uart_vtable};
    h2_mqtt_bk_ledger_console_t bk_console={.uart=&uart,.timeout_ms=5000u,.writable=1};
    const h2_pal_log_vtable_t log_vtable={.write=log_record};
    const h2_pal_log_api_t log={.user=emit_bk?(void *)&bk_console:(void *)&console,.vtable=&log_vtable};
    const h2_pal_firmware_info_vtable_t firmware_vtable={.get_current=firmware};
    const h2_pal_firmware_info_api_t image={.user=state,.vtable=&firmware_vtable};
    const h2_pal_time_vtable_t time_vtable={.sleep_ms=sleep_ms};
    const h2_pal_time_api_t time={.vtable=&time_vtable};
    h2_runtime_t runtime={.log=&log,.firmware_info=&image,.time=&time};
    h2_mqtt_device_result_t result={.rc=suite_error};
    strcpy(result.execution,execution_id);
    memset(result.ca_sha256,'c',64u);result.ca_sha256[64]='\0';result.epoch_ms=123u;
    result.suite.selected=result.suite.passed=36u;
    for(unsigned i=0u;i<36u;++i){result.suite.cases[i].id=case_ids[i];result.suite.cases[i].passed=1;}
    if(emit_ledger){
        char boot[256];
        (void)snprintf(boot,sizeof(boot),"H2_PAL_MQTT_BOOT id=%s version=test-version epoch_ms=123 ca_sha256=%s",result.execution,result.ca_sha256);
        assert(log_record(log.user,H2_PAL_LOG_INFO,"pal-mqtt",boot)==0);
    }
    int rc;
    if(emit_bk){
        rc=h2_mqtt_device_replay(&runtime,&result);
        if(rc==0)rc=h2_mqtt_bk_console_finish(&bk_console,result.rc,confirm,state);
    }else rc=h2_mqtt_esp_console_complete(&console,&runtime,&result,0,confirm,state);
    if(emit_ledger){
        rewind(state->wire);
        char bytes[1024];size_t count;
        while((count=fread(bytes,1u,sizeof(bytes),state->wire))!=0u)assert(fwrite(bytes,1u,count,stdout)==count);
    }
    assert(fclose(state->wire)==0);
    return rc;
}

int main(int argc,char **argv) {
    if(argc==3 && strcmp(argv[1],"--emit-ledger")==0){
        emit_ledger=1;emit_bk=strcmp(argv[2],"bk7258")==0;
        observed_t state={0};return execute(&state,0);
    }
    for(unsigned boundary=1u;boundary<=39u;++boundary){
        observed_t write_failure={.fail_write=boundary};
        assert(execute(&write_failure,0)==H2_PAL_ERR_TIMEOUT && write_failure.confirms==0u);
        observed_t prefix={.short_write=boundary};
        assert(execute(&prefix,0)==H2_PAL_ERR_IO && prefix.confirms==0u);
        observed_t flush_failure={.fail_flush=boundary};
        assert(execute(&flush_failure,0)==H2_PAL_ERR_TIMEOUT && flush_failure.confirms==0u);
    }
    observed_t good={0};assert(execute(&good,0)==0 && good.confirms==1u && good.writes==40u && good.flushes==40u);
    observed_t bad_suite={0};assert(execute(&bad_suite,H2_PAL_ERR_IO)==H2_PAL_ERR_IO && bad_suite.confirms==0u);
    observed_t bad_image={.firmware_error=H2_PAL_ERR_IO};assert(execute(&bad_image,0)==H2_PAL_ERR_IO && bad_image.confirms==0u);
    observed_t bad_confirm={.confirm_error=H2_PAL_ERR_IO};assert(execute(&bad_confirm,0)==H2_PAL_ERR_IO && bad_confirm.confirms==1u);
    observed_t bad_terminal={.fail_flush=40u};assert(execute(&bad_terminal,0)==H2_PAL_ERR_TIMEOUT && bad_terminal.confirms==1u);
    return 0;
}
