#include "h2_ios_mqtt.h"
#include "h2_ios_net.h"
#include <assert.h>
#include <stdlib.h>
static int live, net_refs, fail_after = -1, allocations, busy;
struct h2_ios_net { int active; };
static struct h2_ios_net network;
static void *allocate(void *user, size_t size) {
    (void)user;
    if (fail_after >= 0 && allocations++ >= fail_after) return NULL;
    void *pointer = malloc(size); if (pointer) ++live; return pointer;
}
static void *reallocate(void *user, void *pointer, size_t size) {
    if (!pointer) return allocate(user, size);
    return realloc(pointer, size);
}
static void release(void *user, void *pointer) {(void)user;if(pointer){--live;free(pointer);}}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc=allocate,.realloc=reallocate,.free=release};
static const h2_pal_mem_api_t memory = {.vtable=&memory_vtable};
static h2_pal_result_t monotonic(void *user,uint64_t *out){(void)user;*out=1u;return H2_PAL_OK;}
static const h2_pal_time_vtable_t time_vtable={.get_monotonic_ms=monotonic};
static const h2_pal_time_api_t clock_api={.vtable=&time_vtable};
static const h2_pal_net_api_t net_api={0};
const h2_pal_mem_api_t *h2_ios_platform_mem_api(void){return &memory;}
const h2_pal_time_api_t *h2_ios_platform_time_api(void){return &clock_api;}
h2_pal_result_t h2_ios_net_create(h2_ios_net_t **out){++net_refs;*out=&network;return H2_PAL_OK;}
const h2_pal_net_api_t *h2_ios_net_api(h2_ios_net_t *owner){assert(owner==&network);return &net_api;}
h2_pal_result_t h2_ios_net_destroy(h2_ios_net_t **out){if(!*out)return H2_PAL_OK;if(busy)return H2_PAL_ERR_INVALID_STATE;--net_refs;*out=NULL;return H2_PAL_OK;}
int main(void){
    h2_ios_mqtt_t *owner=(h2_ios_mqtt_t *)(uintptr_t)1u;
    assert(h2_ios_mqtt_create(NULL,NULL)==H2_PAL_ERR_INVALID_ARG);
    h2_pal_mem_api_t invalid={0};
    assert(h2_ios_mqtt_create(&invalid,&owner)==H2_PAL_ERR_INVALID_ARG && owner==NULL);
    for(int failure=0;failure<2;++failure){
        fail_after=failure;allocations=0;
        assert(h2_ios_mqtt_create(NULL,&owner)==H2_PAL_ERR_NO_MEMORY && owner==NULL);
        assert(live==0 && net_refs==0);
    }
    fail_after=-1;
    assert(h2_ios_mqtt_create(NULL,&owner)==H2_PAL_OK && owner);
    const h2_pal_mqtt_api_t *api=h2_ios_mqtt_api(owner);
    uint8_t buffer[1024];
    h2_pal_mqtt_client_config_t config={.endpoint={{"localhost",9u},1883u},.client_id={"lifetime",8u},
        .network_buffer=buffer,.network_buffer_len=sizeof(buffer)};
    h2_pal_mqtt_client_t *client=NULL;
    assert(h2_pal_mqtt_open(api,&config,&client)==H2_PAL_OK && client);
    h2_pal_mqtt_close(api,client);
    int baseline=live;
    h2_ios_mqtt_t *original=owner;
    busy=1;
    assert(h2_ios_mqtt_destroy(&owner)==H2_PAL_ERR_INVALID_STATE && owner==original);
    assert(h2_ios_mqtt_api(owner)==api && live==baseline && net_refs==1);
    busy=0;
    assert(h2_ios_mqtt_destroy(&owner)==H2_PAL_OK && owner==NULL);
    assert(live==0 && net_refs==0);
    assert(h2_ios_mqtt_destroy(&owner)==H2_PAL_OK);
    return 0;
}
