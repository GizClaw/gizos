#include "../src/modules/h2_lua_vector_cg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "h2_lua_vector_delta_cases.h"

static uint8_t data[4096];static size_t length;
static void byte(unsigned v){assert(length<sizeof(data));data[length++]=(uint8_t)v;}
static void real(float v){uint32_t n;memcpy(&n,&v,4);for(int i=0;i<4;i++)byte(n>>(i*8));}
static void point(unsigned op,float x,float y){byte(op);real(x);real(y);}
static void begin(void){const uint8_t header[]={'H','2','V','G',8,0,8,0,0,0,1,0};memcpy(data,header,sizeof(header));length=sizeof(header);}
static void rectangle(float x,float y,float w,float h){byte(4);point(5,x,y);point(6,x+w,y);point(6,x+w,y+h);point(6,x,y+h);byte(8);}
static void red(void){byte(10);byte(0);byte(255);byte(255);byte(255);byte(0);byte(0);byte(255);real(1);real(1);}
int main(void){
  test_vector_delta(h2_lua_vector_cg_render);
  uint8_t rgba[8*8*4],again[sizeof(rgba)];double identity[]={1,0,0,1,0,0};
  begin();rectangle(1,1,6,6);red();byte(0);
  assert(h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  assert(rgba[(4*8+4)*4]==255&&rgba[(4*8+4)*4+3]==255);
  assert(rgba[3]==0);
  assert(h2_lua_vector_cg_render(data,length,again,8,8,identity));
  assert(!memcmp(rgba,again,sizeof(rgba)));
  for(size_t n=0;n<length;n++)assert(!h2_lua_vector_cg_render(data,n,rgba,8,8,identity));
  begin();byte(1);rectangle(0,0,4,8);byte(11);rectangle(0,0,8,8);red();byte(2);byte(0);
  assert(h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  assert(rgba[(4*8+2)*4+3]==255&&rgba[(4*8+6)*4+3]==0);
  begin();for(int i=0;i<33;i++)byte(1);byte(0);
  assert(!h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  begin();byte(2);byte(0);assert(!h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  begin();byte(4);point(5,NAN,0);byte(0);assert(!h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  begin();byte(99);byte(0);assert(!h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  begin();byte(4);byte(13);byte(4);byte(0);
  const unsigned xy[]={4,4,28,4,28,28,4,28};
  for(unsigned i=0;i<8;i++){byte(xy[i]);byte(0);}red();byte(0);
  assert(h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  assert(rgba[(4*8+4)*4]==255&&rgba[3]==0);
  for(size_t n=0;n<length;n++)assert(!h2_lua_vector_cg_render(data,n,rgba,8,8,identity));
  begin();byte(4);byte(13);byte(255);byte(255);byte(0);
  assert(!h2_lua_vector_cg_render(data,length,rgba,8,8,identity));
  puts("H2VG: visible fill, clip, determinism, truncation, stack bounds and invalid commands passed");
  return 0;
}
