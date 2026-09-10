#include "h2_lua_vm.h"
#include "qi_duel_retro_audio_generated.h"
#include "qi_duel_retro_score_generated.h"
#include "qi_duel_retro_audio_test_script_generated.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load(h2_lua_vm_t *vm, const char *name, const uint8_t *data,
                 size_t length, h2_lua_vm_execution_t *execution) {
  size_t capacity = length + strlen(name) + 64;
  char *source = malloc(capacity);
  assert(source);
  int prefix = snprintf(source, capacity, "%s=(function()\n", name);
  memcpy(source + prefix, data, length);
  const char *suffix = "\nend)()";
  memcpy(source + prefix + length, suffix, strlen(suffix));
  h2_lua_vm_result_t result = h2_lua_vm_execute_text(
      vm, name, (const uint8_t *)source, prefix + length + strlen(suffix), execution);
  if (result != H2_LUA_VM_OK) fprintf(stderr, "%s\n", execution->error);
  assert(result == H2_LUA_VM_OK);
  free(source);
}

int main(void) {
  h2_lua_vm_t *vm = NULL;
  const h2_lua_vm_config_t config = {.memory_limit_bytes=1024*1024,
      .source_limit_bytes=8*1024*1024, .output_limit_bytes=1024};
  assert(h2_lua_vm_create(&config,&vm)==H2_LUA_VM_OK);
  char output[1024]={0}, error[1024]={0};
  h2_lua_vm_execution_t execution = {.output=output,.output_capacity=sizeof(output),
      .error=error,.error_capacity=sizeof(error)};
  load(vm,"Retro",qi_duel_retro_audio,qi_duel_retro_audio_size,&execution);
  load(vm,"Score",qi_duel_retro_score,qi_duel_retro_score_size,&execution);
  h2_lua_vm_result_t result = h2_lua_vm_execute_text(vm,"bgm_test",
      qi_duel_retro_audio_test,qi_duel_retro_audio_test_size,&execution);
  if (result != H2_LUA_VM_OK) fprintf(stderr,"%s\n",error);
  assert(result==H2_LUA_VM_OK && strcmp(output,"ok")==0);
  h2_lua_vm_close(vm);
  return 0;
}
