#include "h2_lua_vm.h"
#include "qi_duel_rules_generated.h"
#include "qi_duel_rules_test_script_generated.h"
#include "qi_duel_link_protocol_generated.h"
#include "qi_duel_link_test_script_generated.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  h2_lua_vm_t *vm=NULL;
  const h2_lua_vm_config_t config={.memory_limit_bytes=1024*1024,
      .source_limit_bytes=128*1024,.output_limit_bytes=1024};
  assert(h2_lua_vm_create(&config,&vm)==H2_LUA_VM_OK);
  const char *prefix="Rules=(function()\n",*suffix="\nend)()";
  size_t size=strlen(prefix)+qi_duel_rules_size+strlen(suffix);
  char *source=malloc(size+1);assert(source);
  memcpy(source,prefix,strlen(prefix));
  memcpy(source+strlen(prefix),qi_duel_rules,qi_duel_rules_size);
  memcpy(source+strlen(prefix)+qi_duel_rules_size,suffix,strlen(suffix));
  char output[1024]={0},error[1024]={0};
  h2_lua_vm_execution_t execution={.output=output,.output_capacity=sizeof(output),
      .error=error,.error_capacity=sizeof(error)};
  assert(h2_lua_vm_execute_text(vm,"rules",(const uint8_t *)source,size,&execution)==H2_LUA_VM_OK);
  free(source);
  h2_lua_vm_result_t result=h2_lua_vm_execute_text(vm,"rules_test",qi_duel_rules_test,
      qi_duel_rules_test_size,&execution);
  if(result!=H2_LUA_VM_OK)fprintf(stderr,"%s\n",error);
  assert(result==H2_LUA_VM_OK && strcmp(output,"ok")==0);
  prefix="package.preload.rules=function() return Rules end\nProtocol=(function()\n";
  size=strlen(prefix)+qi_duel_link_protocol_size+strlen(suffix);
  source=malloc(size+1);assert(source);
  memcpy(source,prefix,strlen(prefix));
  memcpy(source+strlen(prefix),qi_duel_link_protocol,qi_duel_link_protocol_size);
  memcpy(source+strlen(prefix)+qi_duel_link_protocol_size,suffix,strlen(suffix));
  assert(h2_lua_vm_execute_text(vm,"protocol",(const uint8_t *)source,size,&execution)==H2_LUA_VM_OK);
  free(source);
  result=h2_lua_vm_execute_text(vm,"link_test",qi_duel_link_test,qi_duel_link_test_size,&execution);
  if(result!=H2_LUA_VM_OK)fprintf(stderr,"%s\n",error);
  assert(result==H2_LUA_VM_OK && strcmp(output,"ok")==0);
  h2_lua_vm_close(vm);
  return 0;
}
