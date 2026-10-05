#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "device_runner.h"
#include "ledger_console.h"
#include <common/sys_config.h>
#include "bk_private/bk_init.h"
#include <os/os.h>
#include <driver/uart.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/sha256.h>
static h2_runtime_t *runtime;
static h2_mqtt_device_result_t result;
#if defined(H2_BK_MEM_DIAGNOSTICS) && H2_BK_MEM_DIAGNOSTICS
extern void h2_bk_mqtt_mem_mark(void);
extern void h2_bk_mqtt_mem_report(void);
#endif
static const h2_pal_log_api_t *board_log;
#if !CONFIG_SYS_PRINT_DEV_UART
#error BK MQTT ledger requires the existing AP-owned physical UART provider
#endif
static h2_mqtt_bk_ledger_console_t console;
/* The BK target console policy borrows the real bounded UART writer. SDK
 * asynchronous printf cannot acknowledge delivery of mandatory records. */
static int ledger_log(void *user,h2_pal_log_level_t level,const char *scope,const char *message){
    (void)user;
    if(message==NULL)return H2_PAL_ERR_INVALID_ARG;
    if(scope!=NULL && strcmp(scope,"pal-mqtt")==0)
        return h2_mqtt_bk_console_record(&console,message);
    return h2_pal_log_write(board_log,level,scope,message);
}
static const h2_pal_log_vtable_t ledger_log_vtable={.write=ledger_log};
static const h2_pal_log_api_t ledger_log_api={.vtable=&ledger_log_vtable};
static void hold(void){for(;;)rtos_delay_milliseconds(1000u);}
static void fail(const char *stage,int rc){printf("H2_PAL_MQTT_SETUP_FAIL stage=%s rc=%d\n",stage,rc);fflush(stdout);hold();}
static int snapshot(size_t out[10]){
    h2_bk_platform_resource_stats_t s={0};int rc=h2_bk_platform_get_resource_stats(&s);
    if(rc==H2_PAL_OK){size_t values[]={s.live_tasks,s.task_stack_bytes,s.live_queues,s.live_mutexes,s.live_semaphores,
        s.live_conditions,s.live_timers,s.live_firmware_infos,s.allocations,s.allocation_bytes};memcpy(out,values,sizeof(values));}
    return rc;
}
static int ca_digest(void *user,const uint8_t *bytes,size_t length,uint8_t digest[32]){
    (void)user;return mbedtls_sha256(bytes,length,digest,0)==0?H2_PAL_OK:H2_PAL_ERR_IO;
}
static int confirm_app(void *unused){
    (void)unused;return h2_bk_h2loader_confirm_current_app(runtime);
}
static int restart_commands(void *unused){
    (void)unused;
    return h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(runtime,"pal-mqtt",
        H2_LOADER_CAPABILITY_UART|H2_LOADER_CAPABILITY_WIFI);
}
static int drain_console(void *unused){
    (void)unused;uint32_t started=rtos_get_time();
    while(!bk_uart_is_tx_over(CONFIG_UART_PRINT_PORT)){
        if((uint32_t)(rtos_get_time()-started)>=console.timeout_ms)return H2_PAL_ERR_TIMEOUT;
        rtos_delay_milliseconds(1u);
    }
    return H2_PAL_OK;
}
static int protocol_status(const char *stage,int rc){
    char line[160];int n=snprintf(line,sizeof(line),"H2_PAL_MQTT_SETUP_FAIL stage=%s rc=%d",stage,rc);
    if(n<0 || (size_t)n>=sizeof(line))return H2_PAL_ERR_NO_SPACE;
    return h2_mqtt_bk_console_record(&console,line);
}
static void run(void *unused){
    (void)unused;rtos_delay_milliseconds(5000u);
    int rc;
    for(;;){rc=h2_mqtt_device_prepare(runtime);if(rc==H2_PAL_OK)break;
        if(rc!=H2_PAL_ERR_NOT_FOUND && rc!=H2_PAL_ERR_UNAVAILABLE && rc!=H2_PAL_ERR_TIMEOUT && rc!=H2_PAL_ERR_BUSY)fail("network",rc);
        printf("H2_PAL_MQTT_SETUP_WAIT network=not_ready rc=%d cases_started=0\n",rc);fflush(stdout);rtos_delay_milliseconds(3000u);}
    console.uart=h2_bk_platform_uart_io_stream_api();
    console.timeout_ms=5000u;
    const char *failed_stage=NULL;
    /* Stop joins management and destroys its physical owner. Reconfigure our
     * console before measuring: RX stays out of SDK shell and TX records use
     * the real provider's bounded mutex/FIFO/suspend-resume transaction. */
    rc=h2_bk_h2loader_stop_app_iostreamikcp();
    if(rc!=H2_PAL_OK){failed_stage="commands-stop";goto restore_commands;}
    rc=h2_mqtt_bk_console_begin(&console,console.uart,CONFIG_UART_PRINT_BAUD_RATE,5000u);
    if(rc!=H2_PAL_OK){failed_stage="console-open";goto restore_commands;}
    rc=h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_CONTROL_QUIESCENT stop=0");
    if(rc!=H2_PAL_OK){failed_stage="console-quiescent";goto restore_commands;}
    rc=snapshot(result.before);
    if(rc!=H2_PAL_OK){result.cleanup=rc;failed_stage="before";goto restore_commands;}
#if defined(H2_BK_MEM_DIAGNOSTICS) && H2_BK_MEM_DIAGNOSTICS
    h2_bk_mqtt_mem_mark();
#endif
    /* The real Runtime provider selects eight incoming/outgoing records. Both
     * resource snapshots include the same physical console owner. */
    rc=h2_mqtt_device_run(runtime,8u,ca_digest,NULL,&result);
    int after=snapshot(result.after);
#if defined(H2_BK_MEM_DIAGNOSTICS) && H2_BK_MEM_DIAGNOSTICS
    h2_bk_mqtt_mem_report();
#endif
    result.cleanup=after==H2_PAL_OK && memcmp(result.before,result.after,sizeof(result.before))==0?H2_PAL_OK:H2_PAL_ERR_IO;
    if(rc==H2_PAL_OK && result.cleanup!=H2_PAL_OK)rc=result.cleanup;
    result.rc=rc;
    int ledger=h2_mqtt_device_replay(runtime,&result);
    if(ledger!=H2_PAL_OK){rc=ledger;failed_stage="ledger-delivery";}
    if(console.error!=H2_PAL_OK){rc=console.error;failed_stage="ledger-write";}
restore_commands: ;
    /* Release before restart so old monitor bytes/overflow cannot enter a new
     * control session. Even setup/suite/output failure always attempts restore. */
    int commands=h2_mqtt_bk_console_restore(&console,drain_console,h2_bk_platform_uart_io_stream_deinit,
        restart_commands,NULL);
    if(commands!=H2_PAL_OK){rc=commands;failed_stage="commands-restart";}
    if(rc==H2_PAL_OK && console.error!=H2_PAL_OK){rc=console.error;failed_stage="console-write-or-drain";}
    if(commands==H2_PAL_OK){
        int output=h2_mqtt_bk_console_record(&console,"H2_PAL_MQTT_CONTROL_RESTORED restart=0");
        if(output!=H2_PAL_OK){rc=output;failed_stage="console-restored";}
    }
    if(failed_stage!=NULL)(void)protocol_status(failed_stage,rc);
    result.rc=rc;
    int output=h2_mqtt_bk_console_finish(&console,rc,confirm_app,NULL);
    if(output!=H2_PAL_OK){
        result.rc=output;
        printf("H2_PAL_MQTT_SETUP_FAIL stage=ready-or-confirm rc=%d\n",output);fflush(stdout);
    }
    /* There is one first ledger/terminal. Later replay cannot repair a missing
     * record and cannot replace this boot's actual failed output evidence. */
    hold();
}
static void entry(void *unused){
    (void)unused;puts("H2_PAL_MQTT_PLATFORM_BOOT board=bk7258");fflush(stdout);
    h2_runtime_config_t config={0};int rc=h2_bk7258_board_runtime_config(&config);if(rc!=H2_PAL_OK)fail("board",rc);
    board_log=config.log;config.log=&ledger_log_api;
    rc=h2_runtime_init(&config,&runtime);if(rc!=H2_PAL_OK)fail("runtime",rc);
    rc=h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(runtime,"pal-mqtt",H2_LOADER_CAPABILITY_UART|H2_LOADER_CAPABILITY_WIFI);
    if(rc!=H2_PAL_OK)fail("commands",rc);
    const h2_pal_task_options_t options={.name=h2_pal_mqtt_device_runner_task_name,.min_stack_size=65536u};h2_pal_task_t *task=NULL;
    rc=h2_pal_task_start(runtime->task,&options,run,NULL,&task);if(rc!=H2_PAL_OK)fail("runner",rc);hold();
}
int main(void){int rc=h2_bk_target_task_policy_install();if(rc!=H2_PAL_OK)return -1;bk_init();
    rc=h2_bk7258_board_start_entry_task("bk/pal-mqtt",entry,NULL);if(rc!=H2_PAL_OK)fail("entry",rc);return 0;}
