#include "device_runner.h"
#include <stdio.h>
#include <string.h>

static struct { const char *id; int status, rc; } ledger[30];
static size_t count;
static uint32_t phase, nonce;
static int run_rc, control_rc;
static const char *image_version;
static h2_pal_storage_result_t result;
static const char *names[]={"NOT_RUN","PASS","FAIL","BLOCKED"};
static void record(void *user,const char *id,h2_pal_storage_status_t status,h2_pal_result_t rc) {
  (void)user;
  if (count<30) {ledger[count].id=id;ledger[count].status=status;ledger[count].rc=rc;++count;}
  printf("H2_STORAGE_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%lu}\n",id,names[status],rc,(unsigned)phase,(unsigned long)nonce);
}
static int control(h2_runtime_t *runtime,int save) {
  h2_pal_pref_namespace_t *ns=NULL;
  int rc=h2_pal_pref_open(runtime->pref,"h2storectl",H2_PAL_PREF_OPEN_READ_WRITE,&ns);
  if (rc) return rc;
  if (save) {
    rc=ns->set_u32(ns,"phase",phase+1u);
    if (!rc) rc=ns->set_u32(ns,"nonce",nonce);
    if (!rc) rc=ns->set_string(ns,"version",image_version);
    if (!rc) rc=ns->commit(ns);
  } else {
    char *stored=NULL;
    rc=ns->get_string(ns,runtime->mem,"version",&stored);
    if (rc==H2_PAL_ERR_NOT_FOUND || (rc==H2_PAL_OK && strcmp(stored,image_version))) {
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
int h2_storage_device_run(h2_runtime_t *runtime,const char *directory,const char *version) {
  image_version=version; count=0;
  int rc=control(runtime,0);
  if (rc) return rc;
  printf("H2_STORAGE_BOOT version=%s phase=%u nonce=%lu\n",version,(unsigned)phase,(unsigned long)nonce);
  if (phase==3) {puts("H2_STORAGE_ALREADY_COMPLETE no_new_run=1");return H2_PAL_ERR_INVALID_STATE;}
  if (phase!=1 && phase!=2) return H2_PAL_ERR_INVALID_STATE;
  h2_pal_storage_config_t tests={.root=directory,.namespace_a="h2storea",.namespace_b="h2storeb",.nonce=nonce,.phase=(h2_pal_storage_phase_t)phase,.case_result=record};
  run_rc=h2_pal_storage_e2e_run(runtime,&tests,&result);
  control_rc=run_rc?H2_PAL_ERR_INVALID_STATE:control(runtime,1);
  return run_rc?run_rc:control_rc;
}
void h2_storage_device_replay(h2_runtime_t *runtime) {
  printf("H2_STORAGE_BOOT version=%s phase=%u nonce=%lu replay=1\n",image_version,(unsigned)phase,(unsigned long)nonce);
  for(size_t i=0;i<count;++i) {
    printf("H2_STORAGE_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%lu}\n",ledger[i].id,names[ledger[i].status],ledger[i].rc,(unsigned)phase,(unsigned long)nonce);
    h2_pal_time_sleep_ms(runtime->time,90);
  }
  printf("H2_STORAGE_PHASE {\"version\":\"%s\",\"phase\":%u,\"nonce\":%lu,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"cleanup\":%d,\"rc\":%d,\"control\":%d}\n",image_version,(unsigned)phase,(unsigned long)nonce,(unsigned)result.passed,(unsigned)result.failed,(unsigned)result.blocked,result.cleanup_result,run_rc,control_rc);
}
