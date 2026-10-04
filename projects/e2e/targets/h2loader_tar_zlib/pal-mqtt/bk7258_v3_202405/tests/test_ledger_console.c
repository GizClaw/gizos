#include "ledger_console.h"
#include <assert.h>

/* Fault injection for the launcher policy, not a mocked MQTT E2E provider. */
static int configure_rc, write_rc, restart_rc, drain_rc;
static int configures, writes, drains, releases, restarts;
static size_t accepted, last_length;
static char last_record[1024];
static int configure(void *user,const h2_pal_uart_io_stream_config_t *config){
    (void)user;++configures;
    assert(config->baud_rate==460800u && config->data_bits==8u && config->stop_bits==1u);
    assert(config->parity==H2_PAL_UART_PARITY_NONE && config->flow_control==H2_PAL_UART_FLOW_CONTROL_NONE);
    return configure_rc;
}
static int write_record(void *user,const void *bytes,size_t length,size_t *written,uint32_t timeout){
    (void)user;++writes;assert(timeout==5000u);
    assert(length<=sizeof(last_record));memcpy(last_record,bytes,length);last_length=length;
    *written=accepted<length?accepted:length;return write_rc;
}
static const h2_pal_uart_io_stream_vtable_t vtable={.configure=configure,.write=write_record};
static const h2_pal_uart_io_stream_api_t uart={.vtable=&vtable};
static int drain(void *user){(void)user;++drains;assert(!releases);return drain_rc;}
static void release(void){++releases;assert(!restarts);}
static int restart(void *user){(void)user;++restarts;return restart_rc;}
static void reset(h2_mqtt_bk_ledger_console_t *console){
    memset(console,0,sizeof(*console));configure_rc=write_rc=restart_rc=drain_rc=0;
    configures=writes=drains=releases=restarts=0;accepted=1024u;last_length=0;
}
static void start(h2_mqtt_bk_ledger_console_t *console){
    assert(h2_mqtt_bk_console_begin(console,&uart,460800u,5000u)==H2_PAL_OK);
    assert(configures==1 && console->owned && console->writable);
    assert(!h2_mqtt_bk_console_can_confirm(console,H2_PAL_OK));
}
int main(void){
    h2_mqtt_bk_ledger_console_t console;
    char long_line[512];memset(long_line,'x',sizeof(long_line)-1u);long_line[sizeof(long_line)-1u]='\0';
    reset(&console);start(&console);
    assert(h2_mqtt_bk_console_record(&console,long_line)==H2_PAL_OK && writes==1);
    assert(last_length==513u && memcmp(last_record,long_line,511u)==0 && last_record[511]=='\r' && last_record[512]=='\n');
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0);
    assert(drains==1 && releases==1 && restarts==1 && !console.owned && console.writable);
    assert(h2_mqtt_bk_console_can_confirm(&console,0) && !h2_mqtt_bk_console_can_confirm(&console,H2_PAL_ERR_IO));

    reset(&console);start(&console);accepted=3u;
    assert(h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_CASE {}") == H2_PAL_ERR_IO && writes==1);
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0);
    assert(releases==1 && restarts==1 && console.error==H2_PAL_ERR_IO && !h2_mqtt_bk_console_can_confirm(&console,0));

    reset(&console);start(&console);accepted=3u;write_rc=H2_PAL_ERR_TIMEOUT;
    assert(h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_BOOT id=test") == H2_PAL_ERR_TIMEOUT && writes==1);
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0 && releases==1 && restarts==1);
    accepted=1024u;write_rc=0;
    assert(h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_READY rc=-6 confirm=-2")==0);
    assert(console.error==H2_PAL_ERR_TIMEOUT && !h2_mqtt_bk_console_can_confirm(&console,0));

    reset(&console);start(&console);drain_rc=H2_PAL_ERR_TIMEOUT;
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0);
    assert(drains==1 && releases==1 && restarts==1 && console.error==H2_PAL_ERR_TIMEOUT && !h2_mqtt_bk_console_can_confirm(&console,0));

    reset(&console);configure_rc=H2_PAL_ERR_NO_MEMORY;
    assert(h2_mqtt_bk_console_begin(&console,&uart,460800u,5000u)==H2_PAL_ERR_NO_MEMORY);
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0);
    assert(!releases && !drains && restarts==1 && !h2_mqtt_bk_console_can_confirm(&console,0));

    reset(&console);start(&console);restart_rc=H2_PAL_ERR_BUSY;
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==H2_PAL_ERR_BUSY);
    assert(releases==1 && restarts==1 && !console.writable && !h2_mqtt_bk_console_can_confirm(&console,0));
    assert(h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_READY rc=-7")==H2_PAL_ERR_INVALID_STATE && writes==0);

    reset(&console);start(&console);
    assert(h2_mqtt_bk_console_record(&console,"one\ntwo")==H2_PAL_ERR_INVALID_ARG && writes==0);
    assert(h2_mqtt_bk_console_restore(&console,drain,release,restart,NULL)==0 && releases==1 && restarts==1);
    return 0;
}
