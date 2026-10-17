#include "esp_ledger_console.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char version[]="storage-output-test";
static struct {
    unsigned phase,writes,flushes,commits,control_writes,suite_calls,confirms;
    unsigned fail_write,short_write,fail_flush;
    int pending,confirmed,confirm_rc,commit_rc,suite_rc;
    size_t length,confirm_offset;
    char output[32768];
} fixture;
static const struct {const char *id;unsigned phase;} cases[]={
#define H2_PAL_STORAGE_CASE(id,phase) {id,phase},
#include "h2_pal_storage_cases.inc"
#undef H2_PAL_STORAGE_CASE
};

static int write_record(void *user,const void *record,size_t length,size_t *written,uint32_t timeout) {
    assert(user==&fixture && timeout==5000u && !fixture.pending);
    assert(length>0u && length<H2_STORAGE_DEVICE_RECORD_MAX);
    assert(((const char *)record)[length-1u]=='\n');
    assert(memchr(record,'\n',length-1u)==NULL);
    ++fixture.writes;
    if(fixture.writes==fixture.fail_write)return H2_PAL_ERR_TIMEOUT;
    *written=length-(fixture.writes==fixture.short_write?1u:0u);
    assert(fixture.length+*written<sizeof(fixture.output));
    memcpy(fixture.output+fixture.length,record,*written);
    fixture.length+=*written;fixture.output[fixture.length]='\0';
    fixture.pending=1;
    return H2_PAL_OK;
}
static int flush_record(void *user) {
    assert(user==&fixture && fixture.pending);
    fixture.pending=0;
    ++fixture.flushes;
    return fixture.flushes==fixture.fail_flush?H2_PAL_ERR_IO:H2_PAL_OK;
}
static const h2_pal_uart_io_stream_vtable_t uart_vtable={.write=write_record,.flush=flush_record};
static const h2_pal_uart_io_stream_api_t uart={.user=&fixture,.vtable=&uart_vtable};
static const h2_pal_usb_jtag_io_stream_vtable_t usb_vtable={.write=write_record,.flush=flush_record};
static const h2_pal_usb_jtag_io_stream_api_t usb={.user=&fixture,.vtable=&usb_vtable};
static void release(void *user,void *data) {(void)user;free(data);}
static const h2_pal_mem_vtable_t mem_vtable={.free=release};
static const h2_pal_mem_api_t memory={.vtable=&mem_vtable};
static int sleep_ms(void *user,uint32_t ms) {(void)user;assert(ms==90u);return H2_PAL_OK;}
static const h2_pal_time_vtable_t time_vtable={.sleep_ms=sleep_ms};
static const h2_pal_time_api_t clock_api={.vtable=&time_vtable};
static int close_namespace(h2_pal_pref_namespace_t *ns) {(void)ns;return H2_PAL_OK;}
static int get_string(h2_pal_pref_namespace_t *ns,const h2_pal_mem_api_t *mem,const char *key,char **value) {
    (void)ns;(void)mem;assert(!strcmp(key,"version"));
    if(fixture.phase==1u)return H2_PAL_ERR_NOT_FOUND;
    *value=malloc(sizeof(version));assert(*value!=NULL);memcpy(*value,version,sizeof(version));
    return H2_PAL_OK;
}
static int get_u32(h2_pal_pref_namespace_t *ns,const char *key,uint32_t *value) {
    (void)ns;
    if(!strcmp(key,"contract"))*value=H2_PAL_STORAGE_CONTRACT_VERSION;
    else if(!strcmp(key,"phase"))*value=fixture.phase;
    else {assert(!strcmp(key,"nonce"));*value=123u;}
    return H2_PAL_OK;
}
static int set_u32(h2_pal_pref_namespace_t *ns,const char *key,uint32_t value) {
    (void)ns;(void)key;(void)value;++fixture.control_writes;return H2_PAL_OK;
}
static int set_string(h2_pal_pref_namespace_t *ns,const char *key,const char *value) {
    (void)ns;assert(!strcmp(key,"version") && !strcmp(value,version));
    ++fixture.control_writes;return H2_PAL_OK;
}
static int commit(h2_pal_pref_namespace_t *ns) {
    (void)ns;assert(!fixture.pending && fixture.suite_calls==1u);
    ++fixture.commits;return fixture.commit_rc;
}
static h2_pal_pref_namespace_t control_namespace={.close=close_namespace,.get_string=get_string,
    .get_u32=get_u32,.set_u32=set_u32,.set_string=set_string,.commit=commit};
static int open_namespace(void *user,const char *name,h2_pal_pref_open_mode_t mode,h2_pal_pref_namespace_t **out) {
    (void)user;assert(!strcmp(name,"h2storectl") && mode==H2_PAL_PREF_OPEN_READ_WRITE);
    *out=&control_namespace;return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable={.open=open_namespace};
static const h2_pal_pref_api_t preferences={.vtable=&pref_vtable};
static h2_runtime_t runtime={.pref=&preferences,.mem=&memory,.time=&clock_api};

/* Exercise the actual device control/console around scripted suite outcomes;
 * real FS/Pref cases remain covered by the separate device_lifecycle_test. */
h2_pal_result_t h2_pal_storage_e2e_run(h2_runtime_t *r,const h2_pal_storage_config_t *config,
    h2_pal_storage_result_t *result) {
    assert(r==&runtime && fixture.commits==0u);
    assert(!strcmp(config->root,"/data/pal-storage"));
    ++fixture.suite_calls;
    memset(result,0,sizeof(*result));
    for(size_t i=0u;i<sizeof(cases)/sizeof(cases[0]);++i) {
        if(cases[i].phase!=(unsigned)config->phase)continue;
        if(config->case_result)config->case_result(config->user,cases[i].id,H2_PAL_STORAGE_PASS,H2_PAL_OK);
        ++result->passed;
    }
    return fixture.suite_rc;
}
static int confirm(void *user) {
    assert(user==&fixture && !fixture.pending);
    assert(strstr(fixture.output,fixture.phase==4u?"H2_STORAGE_ALREADY_COMPLETE":"H2_STORAGE_PHASE"));
    fixture.confirm_offset=fixture.length;
    ++fixture.confirms;fixture.confirmed=fixture.confirm_rc==H2_PAL_OK;
    return fixture.confirm_rc;
}
static h2_storage_esp_console_t setup(unsigned phase,int use_usb) {
    memset(&fixture,0,sizeof(fixture));fixture.phase=phase;
    h2_storage_esp_console_t console={.timeout_ms=5000u};
    if(use_usb)console.usb=&usb;else console.uart=&uart;
    return console;
}
static int run(h2_storage_esp_console_t *console) {
    return h2_storage_device_run_with_output(&runtime,"/data/pal-storage",version,
        h2_storage_esp_console_record,console);
}
static unsigned phase_count(unsigned phase) {return phase==1u?31u:phase==2u?3u:phase==3u?2u:0u;}

static void test_success(void) {
    for(int use_usb=0;use_usb<2;++use_usb)for(unsigned phase=1u;phase<=4u;++phase) {
        h2_storage_esp_console_t console=setup(phase,use_usb);
        assert(run(&console)==H2_PAL_OK);
        assert(fixture.commits==(phase==4u?0u:1u));
        assert(strstr(fixture.output,"H2_STORAGE_BOOT contract=2"));
        assert(strstr(fixture.output,"replay=1")==NULL && fixture.confirms==0u);
        assert(h2_storage_esp_console_complete(&console,&runtime,H2_PAL_OK,confirm,&fixture)==H2_PAL_OK);
        assert(fixture.confirmed && fixture.confirms==1u && fixture.writes==fixture.flushes);
        assert(strstr(fixture.output+fixture.confirm_offset,"H2_STORAGE_READY rc=0 confirm=0\n"));
        unsigned rows=0u;const char *row=fixture.output;
        while((row=strstr(row,"H2_STORAGE_CASE "))!=NULL) {++rows;++row;}
        assert(rows==2u*phase_count(phase));
    }
}
static void test_early_output_failure(void) {
    for(unsigned mode=0u;mode<3u;++mode)for(unsigned index=1u;index<=32u;index+=31u) {
        h2_storage_esp_console_t console=setup(1u,0);
        if(mode==0u)fixture.fail_write=index;
        else if(mode==1u)fixture.short_write=index;
        else fixture.fail_flush=index;
        int expected=mode==0u?H2_PAL_ERR_TIMEOUT:H2_PAL_ERR_IO;
        assert(run(&console)==expected);
        assert(fixture.suite_calls==(index==1u?0u:1u));
        assert(fixture.commits==0u && fixture.control_writes==0u && fixture.confirms==0u);
        unsigned writes=fixture.writes;
        assert(h2_storage_device_replay_checked(&runtime)==expected && fixture.writes==writes);
        assert(h2_storage_esp_console_complete(&console,&runtime,expected,confirm,&fixture)==expected);
        assert(fixture.confirms==0u);
    }
}
static void test_replay_and_late_ready_failures(void) {
    for(unsigned mode=0u;mode<3u;++mode) {
        h2_storage_esp_console_t console=setup(1u,1);
        assert(run(&console)==H2_PAL_OK && fixture.commits==1u);
        if(mode==0u)fixture.short_write=fixture.writes+1u;
        else if(mode==1u)fixture.fail_flush=fixture.flushes+33u;
        else fixture.fail_flush=fixture.flushes+34u;
        assert(h2_storage_esp_console_complete(&console,&runtime,H2_PAL_OK,confirm,&fixture)==H2_PAL_ERR_IO);
        assert(fixture.commits==1u);
        assert(fixture.confirms==(mode==2u?1u:0u) && fixture.confirmed==(mode==2u));
        unsigned writes=fixture.writes;
        assert(h2_storage_esp_console_complete(&console,&runtime,H2_PAL_OK,confirm,&fixture)==H2_PAL_ERR_IO);
        assert(fixture.writes==writes);
    }
}
static void test_business_control_and_confirm_errors(void) {
    for(unsigned mode=0u;mode<3u;++mode) {
        h2_storage_esp_console_t console=setup(1u,0);
        if(mode==0u)fixture.suite_rc=-1000;
        else if(mode==1u)fixture.commit_rc=-1000;
        else fixture.confirm_rc=-1000;
        int rc=run(&console);
        assert(h2_storage_esp_console_complete(&console,&runtime,rc,confirm,&fixture)==-1000);
        assert(fixture.confirms==(mode==2u?1u:0u) && !fixture.confirmed);
    }
}
static void test_invalid_console(void) {
    h2_storage_esp_console_t console=setup(1u,0);console.usb=&usb;
    assert(run(&console)==H2_PAL_ERR_INVALID_ARG && fixture.writes==0u && fixture.commits==0u);
    console=setup(1u,0);console.uart=NULL;
    assert(run(&console)==H2_PAL_ERR_INVALID_ARG && fixture.writes==0u);
    assert(h2_storage_device_run_with_output(&runtime,"/data/pal-storage",version,NULL,NULL)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_storage_device_run_with_output(NULL,"/data/pal-storage",version,h2_storage_esp_console_record,&console)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_storage_device_run_with_output(&runtime,NULL,version,h2_storage_esp_console_record,&console)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_storage_device_run_with_output(&runtime,"/data/pal-storage",NULL,h2_storage_esp_console_record,&console)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_storage_device_replay_checked(NULL)==H2_PAL_ERR_INVALID_ARG);
    char long_version[H2_STORAGE_DEVICE_RECORD_MAX+1u];memset(long_version,'x',sizeof(long_version)-1u);long_version[sizeof(long_version)-1u]='\0';
    console=setup(1u,0);
    assert(h2_storage_device_run_with_output(&runtime,"/data/pal-storage",long_version,h2_storage_esp_console_record,&console)==H2_PAL_ERR_TRUNCATED);
    assert(fixture.writes==0u && fixture.commits==0u);
}
int main(void) {
    test_success();test_early_output_failure();test_replay_and_late_ready_failures();
    test_business_control_and_confirm_errors();test_invalid_console();
    return 0;
}
