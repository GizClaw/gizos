#include "device_runner.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct { const char *id; int status, rc; } ledger[36];
static size_t count;
static uint32_t phase, nonce;
static int run_rc, control_rc;
static const char *image_version;
static h2_pal_storage_result_t result;
static h2_storage_device_output_fn output_record;
static void *output_user;
static int output_rc;
static const char *names[]={"NOT_RUN","PASS","FAIL","BLOCKED"};
static int emit(const char *format,...) {
  if(output_rc)return output_rc;
  va_list args;va_start(args,format);
  char line[H2_STORAGE_DEVICE_RECORD_MAX];
  int length=vsnprintf(line,sizeof(line),format,args);
  va_end(args);
  if(length<0 || (size_t)length>=sizeof(line))output_rc=H2_PAL_ERR_TRUNCATED;
  else if(output_record!=NULL)output_rc=output_record(output_user,line,(size_t)length);
  else if(printf("%s",line)<0)output_rc=H2_PAL_ERR_IO;
  return output_rc;
}
static void record(void *user,const char *id,h2_pal_storage_status_t status,h2_pal_result_t rc) {
  (void)user;
  if (count<sizeof(ledger)/sizeof(ledger[0])) {ledger[count].id=id;ledger[count].status=status;ledger[count].rc=rc;++count;}
  /* Finish the suite's owned cleanup; emit latches failure before phase save. */
  (void)emit("H2_STORAGE_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%lu}\n",id,names[status],rc,(unsigned)phase,(unsigned long)nonce);
}
static int control(h2_runtime_t *runtime,int save) {
  h2_pal_pref_namespace_t *ns=NULL;
  int rc=h2_pal_pref_open(runtime->pref,"h2storectl",H2_PAL_PREF_OPEN_READ_WRITE,&ns);
  if (rc) return rc;
  if (save) {
    rc=ns->set_u32(ns,"phase",phase+1u);
    if (!rc) rc=ns->set_u32(ns,"nonce",nonce);
    if (!rc) rc=ns->set_string(ns,"version",image_version);
    if (!rc) rc=ns->set_u32(ns,"contract",H2_PAL_STORAGE_CONTRACT_VERSION);
    if (!rc) rc=ns->commit(ns);
  } else {
    char *stored=NULL;
    uint32_t stored_contract=0;
    rc=ns->get_string(ns,runtime->mem,"version",&stored);
    if (!rc) rc=ns->get_u32(ns,"contract",&stored_contract);
    if (rc==H2_PAL_ERR_NOT_FOUND || (rc==H2_PAL_OK &&
        (strcmp(stored,image_version) || stored_contract!=H2_PAL_STORAGE_CONTRACT_VERSION))) {
      phase=1; nonce=2166136261u;
      for (const unsigned char *p=(const unsigned char*)image_version;*p;++p) nonce=(nonce^*p)*16777619u;
      rc=H2_PAL_OK;
    } else if (!rc) {
      rc=ns->get_u32(ns,"phase",&phase);
      if (!rc) rc=ns->get_u32(ns,"nonce",&nonce);
    }
    h2_pal_mem_free(runtime->mem,stored);
  }
  int close_rc=ns->close(ns);
  return rc?rc:close_rc;
}
static int run(h2_runtime_t *runtime,const char *directory,const char *version,
    h2_storage_device_output_fn output,void *user) {
  if(runtime==NULL || directory==NULL || version==NULL)return H2_PAL_ERR_INVALID_ARG;
  output_record=output;output_user=user;output_rc=H2_PAL_OK;
  image_version=version; count=0;
  memset(&result,0,sizeof(result));
  run_rc=H2_PAL_ERR_INVALID_STATE;
  control_rc=control(runtime,0);
  int rc=control_rc;
  if (rc) return rc;
  rc=emit("H2_STORAGE_BOOT contract=%u version=%s phase=%u nonce=%lu\n",H2_PAL_STORAGE_CONTRACT_VERSION,version,(unsigned)phase,(unsigned long)nonce);
  if(rc)return run_rc=rc;
  if (phase<1 || phase>4) return H2_PAL_ERR_INVALID_STATE;
  h2_pal_storage_config_t tests={.root=directory,.namespace_a="h2storea",.namespace_b="h2storeb",.nonce=nonce,.phase=phase==4?H2_PAL_STORAGE_CLEAN_VERIFY:(h2_pal_storage_phase_t)phase,.case_result=phase==4?NULL:record};
  run_rc=h2_pal_storage_e2e_run(runtime,&tests,&result);
  if(output_rc)run_rc=output_rc;
  if (phase==4) {
    control_rc=0;
    rc=emit("H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=%d rc=%d\n",!run_rc,run_rc);
    return run_rc?run_rc:rc;
  }
  control_rc=run_rc?H2_PAL_ERR_INVALID_STATE:control(runtime,1);
  return run_rc?run_rc:control_rc;
}
int h2_storage_device_run(h2_runtime_t *runtime,const char *directory,const char *version) {
  return run(runtime,directory,version,NULL,NULL);
}
int h2_storage_device_run_with_output(h2_runtime_t *runtime,const char *directory,
    const char *version,h2_storage_device_output_fn output,void *user) {
  if(output==NULL)return H2_PAL_ERR_INVALID_ARG;
  return run(runtime,directory,version,output,user);
}
int h2_storage_device_replay_checked(h2_runtime_t *runtime) {
  if(runtime==NULL)return H2_PAL_ERR_INVALID_ARG;
  if(image_version==NULL)return H2_PAL_ERR_INVALID_STATE;
  int rc=emit("H2_STORAGE_BOOT contract=%u version=%s phase=%u nonce=%lu replay=1\n",H2_PAL_STORAGE_CONTRACT_VERSION,image_version,(unsigned)phase,(unsigned long)nonce);
  if(rc)return rc;
  if (phase==4) {
    return emit("H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=%d rc=%d\n",!run_rc,run_rc);
  }
  for(size_t i=0;i<count;++i) {
    rc=emit("H2_STORAGE_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%lu}\n",ledger[i].id,names[ledger[i].status],ledger[i].rc,(unsigned)phase,(unsigned long)nonce);
    if(rc)return rc;
    rc=h2_pal_time_sleep_ms(runtime->time,90);
    if(rc)return output_rc=rc;
  }
  return emit("H2_STORAGE_PHASE {\"contract\":%u,\"version\":\"%s\",\"phase\":%u,\"nonce\":%lu,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"cleanup\":%d,\"rc\":%d,\"control\":%d}\n",H2_PAL_STORAGE_CONTRACT_VERSION,image_version,(unsigned)phase,(unsigned long)nonce,(unsigned)result.passed,(unsigned)result.failed,(unsigned)result.blocked,result.cleanup_result,run_rc,control_rc);
}
void h2_storage_device_replay(h2_runtime_t *runtime) {
  (void)h2_storage_device_replay_checked(runtime);
}
