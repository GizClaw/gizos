#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "device_runner.h"
#include "bk_private/bk_init.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>
static h2_runtime_t *runtime;
static h2_mqtt_device_result_t result;
static void hold(void){for(;;)rtos_delay_milliseconds(1000u);}
static void fail(const char *stage,int rc){printf("H2_PAL_MQTT_SETUP_FAIL stage=%s rc=%d\n",stage,rc);fflush(stdout);hold();}
static int snapshot(size_t out[10]){
    h2_bk_platform_resource_stats_t s={0};int rc=h2_bk_platform_get_resource_stats(&s);
    if(rc==H2_PAL_OK){size_t values[]={s.live_tasks,s.task_stack_bytes,s.live_queues,s.live_mutexes,s.live_semaphores,
        s.live_conditions,s.live_timers,s.live_firmware_infos,s.allocations,s.allocation_bytes};memcpy(out,values,sizeof(values));}
    return rc;
}
static void run(void *unused){
    (void)unused;rtos_delay_milliseconds(5000u);
    int rc;
    for(;;){rc=h2_mqtt_device_prepare(runtime);if(rc==H2_PAL_OK)break;
        if(rc!=H2_PAL_ERR_NOT_FOUND && rc!=H2_PAL_ERR_UNAVAILABLE && rc!=H2_PAL_ERR_TIMEOUT && rc!=H2_PAL_ERR_BUSY)fail("network",rc);
        printf("H2_PAL_MQTT_SETUP_WAIT network=not_ready rc=%d cases_started=0\n",rc);fflush(stdout);rtos_delay_milliseconds(3000u);}
    rc=snapshot(result.before);if(rc!=H2_PAL_OK)fail("before",rc);
    /* The real BK Runtime provider selects eight incoming/outgoing records. */
    rc=h2_mqtt_device_run(runtime,8u,&result);
    int after=snapshot(result.after);
    result.cleanup=after==H2_PAL_OK && memcmp(result.before,result.after,sizeof(result.before))==0?H2_PAL_OK:H2_PAL_ERR_IO;
    if(rc==H2_PAL_OK && result.cleanup!=H2_PAL_OK)rc=result.cleanup;
    result.rc=rc;
    int confirm=rc==H2_PAL_OK?h2_bk_h2loader_confirm_current_app(runtime):H2_PAL_ERR_INVALID_STATE;
    for(;;){h2_mqtt_device_replay(runtime,&result);printf("H2_PAL_MQTT_READY board=bk7258 rc=%d confirm=%d\n",rc,confirm);fflush(stdout);rtos_delay_milliseconds(5000u);}
}
static void entry(void *unused){
    (void)unused;puts("H2_PAL_MQTT_PLATFORM_BOOT board=bk7258");fflush(stdout);
    h2_runtime_config_t config={0};int rc=h2_bk7258_board_runtime_config(&config);if(rc!=H2_PAL_OK)fail("board",rc);
    rc=h2_runtime_init(&config,&runtime);if(rc!=H2_PAL_OK)fail("runtime",rc);
    rc=h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(runtime,"pal-mqtt",H2_LOADER_CAPABILITY_UART|H2_LOADER_CAPABILITY_WIFI);
    if(rc!=H2_PAL_OK)fail("commands",rc);
    const h2_pal_task_options_t options={.name=h2_pal_mqtt_device_runner_task_name,.min_stack_size=65536u};h2_pal_task_t *task=NULL;
    rc=h2_pal_task_start(runtime->task,&options,run,NULL,&task);if(rc!=H2_PAL_OK)fail("runner",rc);hold();
}
int main(void){int rc=h2_bk_target_task_policy_install();if(rc!=H2_PAL_OK)return -1;bk_init();
    rc=h2_bk7258_board_start_entry_task("bk/pal-mqtt",entry,NULL);if(rc!=H2_PAL_OK)fail("entry",rc);return 0;}
