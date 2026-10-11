"""Generate a host regression from the actual ESP GAP callbacks.

Only SDK dependencies are stand-ins. Production completion helpers and both GAP
branches are copied verbatim, so delayed CONNECT cannot close a newer operation.
"""
from pathlib import Path
import sys


def extract_function(source, name):
    marker = source.index(name + "(")
    start = source.rfind("\nstatic ", 0, marker) + 1
    assert start > 0
    brace = source.index("{", marker)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


PREFIX = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "h2_esp_ble_connect_role.h"
#define CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 3
#define H2_PAL_OK 0
#define H2_PAL_BLE_INVALID_CONN_HANDLE 65535
#define BLE_GAP_ROLE_MASTER 0
#define BLE_GAP_EVENT_ADV_COMPLETE 1
#define BLE_GAP_EVENT_CONNECT 2
#define H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED 3
#define H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED 4
#define H2_PAL_BLE_ROLE_CENTRAL 5
#define H2_PAL_BLE_ROLE_PERIPHERAL 6
#define H2_ESP_BLE_ADV_STOPPED_BIT 1
#define H2_ESP_BLE_CONNECT_DONE_BIT 2
#define H2_ESP_BLE_ADV_SET_STOPPED_BIT(instance) (4u << (instance))
#define H2_ESP_BLE_LL_DATA_LEN 251
#define H2_ESP_BLE_LL_DATA_TIME_US 2120
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int h2_pal_result_t;
typedef struct {int type;uint8_t value[6];} addr_t;
typedef struct {uint16_t conn_handle;int role;int mtu;addr_t peer_addr;} h2_pal_ble_connection_t;
typedef struct {bool allocated,active;uint8_t instance;} h2_pal_ble_adv_set_t;
typedef struct {h2_pal_ble_adv_set_t *set;int status;} h2_pal_ble_adv_set_event_t;
struct ble_gap_conn_desc {int role;struct {int type;uint8_t val[6];} peer_ota_addr;};
struct ble_gap_event {int type;struct {int status;uint16_t conn_handle;} connect;struct {int instance,reason,num_ext_adv_events;} adv_complete;};
static bool s_h2_esp_ble_adv_active,s_h2_esp_ble_connect_pending;
static uint16_t s_h2_esp_ble_connect_handle,s_h2_esp_ble_last_conn_handle;
static int s_h2_esp_ble_connect_result;
static void *s_h2_esp_ble_events=(void*)1;
#if CONFIG_BT_NIMBLE_EXT_ADV
static h2_pal_ble_adv_set_t s_h2_esp_ble_adv_sets[3];
#endif
static unsigned stops,connects,done_bits;static bool descriptor_central;
static void xEventGroupSetBits(void *group,unsigned bits){assert(group);done_bits|=bits;}
static void h2_esp_ble_post(int type,const void *payload,size_t length){if(type==3)++stops;else{assert(type==4&&length==sizeof(h2_pal_ble_connection_t)&&payload);++connects;}}
#if CONFIG_BT_NIMBLE_EXT_ADV
static int h2_esp_ble_lock_adv(void){return 0;}static void h2_esp_ble_unlock_adv(void){}
#endif
static int ble_gap_conn_find(uint16_t handle,struct ble_gap_conn_desc *desc){assert(handle==7);memset(desc,0,sizeof(*desc));desc->role=descriptor_central?BLE_GAP_ROLE_MASTER:1;return 0;}
static int ble_gap_set_data_len(uint16_t handle,int length,int us){assert(handle==7&&length==251&&us==2120);return 0;}
static int ble_att_mtu(uint16_t handle){assert(handle==7);return 517;}
static int h2_esp_ble_addr_type(int type){return type;}
static int h2_esp_ble_map_rc(int rc){return rc? -4:0;}
'''

MAIN = r'''
static int dispatch(struct ble_gap_event *event,void *arg){switch(event->type){CASES default:return 0;}}
int main(void){
 struct ble_gap_event connect={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=7}};
#if CONFIG_BT_NIMBLE_EXT_ADV
 struct ble_gap_event complete={.type=BLE_GAP_EVENT_ADV_COMPLETE};
 /* A new default operation starts after ADV_COMPLETE and before delayed CONNECT. */
 s_h2_esp_ble_adv_active=true;dispatch(&complete,NULL);assert(!s_h2_esp_ble_adv_active&&stops==1);
 dispatch(&connect,NULL);assert(!s_h2_esp_ble_adv_active&&stops==1&&connects==1);
 s_h2_esp_ble_adv_active=true;dispatch(&connect,NULL);assert(s_h2_esp_ble_adv_active&&stops==1&&connects==2);
 /* The same ordering must not clear a restarted independently owned set. */
 h2_pal_ble_adv_set_t *set=&s_h2_esp_ble_adv_sets[1];set->allocated=true;set->active=true;set->instance=1;stops=0;connects=0;
 complete.adv_complete.instance=1;dispatch(&complete,set);assert(!set->active&&stops==1);set->active=true;dispatch(&connect,set);assert(set->active&&stops==1&&connects==1);
#else
 s_h2_esp_ble_adv_active=true;dispatch(&connect,NULL);assert(!s_h2_esp_ble_adv_active&&stops==1&&connects==1);
#endif
 descriptor_central=true;s_h2_esp_ble_connect_pending=true;s_h2_esp_ble_adv_active=true;done_bits=0;
 dispatch(&connect,NULL);assert(s_h2_esp_ble_adv_active&&!s_h2_esp_ble_connect_pending&&s_h2_esp_ble_connect_result==0&&s_h2_esp_ble_connect_handle==7&&(done_bits&H2_ESP_BLE_CONNECT_DONE_BIT));
 connect.connect.status=13;s_h2_esp_ble_connect_pending=true;dispatch(&connect,NULL);assert(s_h2_esp_ble_adv_active&&!s_h2_esp_ble_connect_pending&&s_h2_esp_ble_connect_result==-4&&s_h2_esp_ble_connect_handle==H2_PAL_BLE_INVALID_CONN_HANDLE);
 return 0;
}
'''

def generate(source):
    helpers = extract_function(source, "h2_esp_ble_complete_advertising")
    helpers += "#if CONFIG_BT_NIMBLE_EXT_ADV\n"
    for name in ("h2_esp_ble_adv_set_valid_unlocked",
                 "h2_esp_ble_complete_adv_set_unlocked",
                 "h2_esp_ble_complete_adv_set"):
        helpers += extract_function(source, name)
    helpers += "#endif\n"
    start = source.index("    case BLE_GAP_EVENT_ADV_COMPLETE:")
    end = source.index("    case BLE_GAP_EVENT_CONN_UPDATE:", start)
    return PREFIX + helpers + MAIN.replace("CASES", source[start:end])


if __name__ == "__main__":
    assert len(sys.argv) == 3
    Path(sys.argv[2]).write_text(generate(Path(sys.argv[1]).read_text()))
