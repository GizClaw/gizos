#include "../src/modules/h2_lua_vector_sw.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t calls,fail_at,live;
void *h2_vector_test_calloc(size_t n,size_t size) {
  if(++calls==fail_at)return NULL;
  void *p=calloc(n,size);if(p)live++;return p;
}
void *h2_vector_test_realloc(void *p,size_t size) {
  if(++calls==fail_at)return NULL;
  int fresh=p==NULL;void *q=realloc(p,size);if(q && fresh)live++;return q;
}
void h2_vector_test_free(void *p) {
  if(p){assert(live>0);live--;free(p);}
}
static int cancel(void *user) {(void)user;return 0;}
int main(void) {
  static const uint8_t data[]={72,50,86,71,4,0,4,0,0,0,1,0,4,13,4,0,0,0,0,0,16,0,0,0,16,0,16,0,0,0,16,0,10,0,255,255,255,0,0,255,0,0,128,63,0,0,128,63,0};
  const double m[]={1,0,0,1,1,1};uint8_t expected[8*8*4],actual[sizeof(expected)];
  assert(h2_lua_vector_sw_render_result(data,sizeof(data),expected,NULL,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_OK);
  size_t allocations=calls;assert(allocations>=6 && !live);
  for(size_t fail=1;fail<=allocations;fail++) {
    calls=0;fail_at=fail;
    assert(h2_lua_vector_sw_render_result(data,sizeof(data),actual,NULL,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_NO_MEMORY);
    assert(!live); /* Cleanup must finish before the caller invokes Lua GC. */
    calls=0;fail_at=0;
    assert(h2_lua_vector_sw_render_result(data,sizeof(data),actual,NULL,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_OK);
    assert(!live && !memcmp(expected,actual,sizeof(actual)));
  }
  uint8_t *rows[8];for(unsigned y=0;y<8;y++)rows[y]=actual+y*32;
  for(size_t fail=1;fail<=allocations;fail++) {
    calls=0;fail_at=fail;
    assert(h2_lua_vector_sw_render_result(data,sizeof(data),NULL,rows,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_NO_MEMORY);
    assert(!live);calls=0;fail_at=0;
    assert(h2_lua_vector_sw_render_result(data,sizeof(data),NULL,rows,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_OK);
    assert(!live && !memcmp(expected,actual,sizeof(actual)));
  }
  calls=0;fail_at=0;
  assert(h2_lua_vector_sw_render_result(data,sizeof(data),actual,rows,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_INVALID);
  assert(!calls && !live);
  assert(h2_lua_vector_sw_render_result(data,3,actual,NULL,8,8,m,NULL,NULL)==H2_LUA_VECTOR_SW_INVALID);
  assert(!calls && !live);
  assert(h2_lua_vector_sw_render_result(data,sizeof(data),actual,NULL,8,8,m,cancel,NULL)==H2_LUA_VECTOR_SW_INTERRUPTED);
  assert(!live);
  size_t cache_bytes=h2_lua_vector_sw_cache_bytes(4096);
  void *storage=malloc(cache_bytes);assert(storage);
  for(size_t fail=1;fail<=allocations;fail++) {
    h2_lua_vector_sw_cache_t *cache=h2_lua_vector_sw_cache_init(storage,cache_bytes);
    assert(cache);calls=0;fail_at=fail;
    assert(h2_lua_vector_sw_render_cached_result(data,sizeof(data),actual,NULL,8,8,m,
        NULL,NULL,cache)==H2_LUA_VECTOR_SW_NO_MEMORY);
    assert(!live);calls=0;fail_at=0;
    /* Incomplete coverage must never be published by a failed render. */
    assert(h2_lua_vector_sw_render_cached_result(data,sizeof(data),actual,NULL,8,8,m,
        NULL,NULL,cache)==H2_LUA_VECTOR_SW_OK);
    assert(!live && !memcmp(expected,actual,sizeof(actual)));
    calls=0;
    assert(h2_lua_vector_sw_render_cached_result(data,sizeof(data),NULL,rows,8,8,m,
        NULL,NULL,cache)==H2_LUA_VECTOR_SW_OK);
    assert(calls<allocations); /* Warm draw bypasses edge/intersection allocation. */
    assert(!live && !memcmp(expected,actual,sizeof(actual)));
  }
  free(storage);
  return 0;
}
