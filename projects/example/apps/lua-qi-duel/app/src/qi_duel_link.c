#include "qi_duel_link.h"
#include "lua.h"
#include "lauxlib.h"
#include <string.h>

#define MESSAGE_MAX 512u
typedef struct message {uint16_t len;uint8_t bytes[MESSAGE_MAX];} message_t;
static uint64_t clock_ms(qi_duel_link_t *l) {
  uint64_t n=0;(void)h2_pal_time_get_monotonic_ms(l->runtime->time,&n);return n;
}
static int session(void *user,h2_bleikcp_t *stream,bool central,
                   bool (*running)(void *),void *run_user) {
  qi_duel_link_t *l=user;
  uint8_t input[MESSAGE_MAX+2];size_t used=0,wanted=2;
  atomic_store(&l->central,central);atomic_store(&l->connected,1);
  int result=H2_PAL_OK;
  while(running(run_user)) {
    message_t message;
    if(h2_pal_queue_recv(l->runtime->queue,l->tx,&message,0)==H2_PAL_OK) {
      uint8_t frame[MESSAGE_MAX+2];frame[0]=(uint8_t)message.len;frame[1]=(uint8_t)(message.len>>8);
      memcpy(frame+2,message.bytes,message.len);
      result=h2_bleikcp_write(stream,frame,message.len+2,100);
      if(result!=H2_PAL_OK)break;
    }
    size_t count=0;
    result=h2_bleikcp_read(stream,input+used,wanted-used,&count,10);
    if(result==H2_PAL_ERR_TIMEOUT || result==H2_PAL_ERR_WOULD_BLOCK)continue;
    if(result!=H2_PAL_OK)break;
    used+=count;
    if(used!=wanted)continue;
    if(wanted==2) {
      wanted=2u+input[0]+((size_t)input[1]<<8);
      if(wanted<=2 || wanted>sizeof(input)){result=H2_PAL_ERR_FORMAT;break;}
    } else {
      message.len=(uint16_t)(wanted-2);memcpy(message.bytes,input+2,message.len);
      result=h2_pal_queue_send(l->runtime->queue,l->rx,&message,0);
      if(result!=H2_PAL_OK)break; /* Fail closed, never silently lose a round. */
      used=0;wanted=2;
    }
  }
  atomic_store(&l->connected,0);
  return result==H2_PAL_ERR_CLOSED ? H2_PAL_OK : result;
}
int qi_duel_link_stop(qi_duel_link_t *l) {
  int result=h2_bloomspeaker_engine_stop(l->engine);
  if(result!=H2_PAL_OK)return result; /* Engine still owns the context on join failure. */
  l->engine=NULL;atomic_store(&l->connected,0);
  h2_pal_queue_destroy(l->runtime->queue,l->tx);l->tx=NULL;
  h2_pal_queue_destroy(l->runtime->queue,l->rx);l->rx=NULL;
  return H2_PAL_OK;
}
static qi_duel_link_t *context(lua_State *s){return lua_touserdata(s,lua_upvalueindex(1));}
static int status_result(lua_State *s,int result){lua_pushboolean(s,result==H2_PAL_OK);lua_pushinteger(s,result);return 2;}
static int pair(lua_State *s) {
  qi_duel_link_t *l=context(s);
  int result=qi_duel_link_stop(l);if(result!=H2_PAL_OK)return status_result(s,result);
  uint8_t probe[16];result=h2_pal_crypto_random(l->runtime->crypto,probe,sizeof(probe));
  if(result!=H2_PAL_OK)return status_result(s,result);
  h2_pal_queue_config_t config={.name="qi-duel/link",.item_size=sizeof(message_t),.item_count=8,.allocator=l->runtime->mem};
  result=h2_pal_queue_create(l->runtime->queue,&config,&l->tx);
  if(result==H2_PAL_OK)result=h2_pal_queue_create(l->runtime->queue,&config,&l->rx);
  h2_bloomspeaker_controller_init(&l->controller,clock_ms(l));
  if(result==H2_PAL_OK)result=h2_bloomspeaker_engine_start(l->runtime,&l->controller,
      &(h2_bloomspeaker_engine_config_t){.product_magic=0xc7,.on_session=session,.session_user=l,
          .pause_management_advertising=l->pause_management_advertising,
          .resume_management_advertising=l->resume_management_advertising,
          .management_advertising_user=l->management_advertising_user},&l->engine);
  if(result==H2_PAL_OK && l->engine==NULL)result=H2_PAL_ERR_UNSUPPORTED;
  if(result!=H2_PAL_OK){(void)qi_duel_link_stop(l);return status_result(s,result);}
  h2_bloomspeaker_controller_long_press(&l->controller,clock_ms(l));
  return status_result(s,H2_PAL_OK);
}
static int stop(lua_State *s){return status_result(s,qi_duel_link_stop(context(s)));}
static int state(lua_State *s) {
  qi_duel_link_t *l=context(s);h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(&l->controller,&snapshot);
  lua_pushstring(s,l->engine?h2_bloomspeaker_state_name(snapshot.state):"idle");
  lua_pushboolean(s,atomic_load(&l->connected));lua_pushboolean(s,atomic_load(&l->central));
  lua_pushinteger(s,snapshot.last_error);return 4;
}
static int send(lua_State *s) {
  qi_duel_link_t *l=context(s);size_t len;const char *data=luaL_checklstring(s,1,&len);
  if(len==0 || len>MESSAGE_MAX)return luaL_error(s,"invalid link message length");
  if(!atomic_load(&l->connected))return status_result(s,H2_PAL_ERR_CLOSED);
  message_t m={.len=(uint16_t)len};memcpy(m.bytes,data,len);
  return status_result(s,h2_pal_queue_send(l->runtime->queue,l->tx,&m,0));
}
static int receive(lua_State *s) {
  qi_duel_link_t *l=context(s);message_t m;
  if(l->rx && h2_pal_queue_recv(l->runtime->queue,l->rx,&m,0)==H2_PAL_OK)lua_pushlstring(s,(char *)m.bytes,m.len);
  else lua_pushnil(s);
  return 1;
}
static void hex(lua_State *s,const uint8_t *bytes,size_t len) {
  char output[64];const char *digits="0123456789abcdef";
  for(size_t i=0;i<len;i++){output[i*2]=digits[bytes[i]>>4];output[i*2+1]=digits[bytes[i]&15];}
  lua_pushlstring(s,output,len*2);
}
static int random_nonce(lua_State *s) {
  uint8_t nonce[16];int r=h2_pal_crypto_random(context(s)->runtime->crypto,nonce,sizeof(nonce));
  if(r!=H2_PAL_OK)return luaL_error(s,"secure randomness unavailable");
  hex(s,nonce,sizeof(nonce));return 1;
}
static int digest(lua_State *s) {
  size_t len;const char *data=luaL_checklstring(s,1,&len);
  if(len>MESSAGE_MAX)return luaL_error(s,"commit too large");
  uint8_t hash[32];static const uint8_t domain[]="QI-DUEL-COMMIT-v1";
  int r=h2_pal_crypto_hkdf_sha256(context(s)->runtime->crypto,(const uint8_t *)data,len,
      domain,sizeof(domain)-1,NULL,0,hash,sizeof(hash));
  if(r!=H2_PAL_OK)return luaL_error(s,"commit digest unavailable");
  hex(s,hash,sizeof(hash));return 1;
}
int qi_duel_link_open(void *state_ptr,void *user) {
  lua_State *s=state_ptr;lua_newtable(s);
  const char *names[]={"pair","stop","state","send","receive","nonce","digest"};
  const lua_CFunction functions[]={pair,stop,state,send,receive,random_nonce,digest};
  for(unsigned i=0;i<sizeof(functions)/sizeof(functions[0]);i++) {
    lua_pushlightuserdata(s,user);lua_pushcclosure(s,functions[i],1);lua_setfield(s,-2,names[i]);
  }
  return 1;
}
