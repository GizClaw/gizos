#include "runner.h"
#include <stdio.h>
#include <unistd.h>

typedef struct reporter { FILE *out; unsigned phase; uint32_t nonce; size_t count; } reporter_t;
static void record(void *user,const char *id,h2_pal_storage_status_t status,h2_pal_result_t rc) {
  reporter_t *r=user;
  const char *names[]={"NOT_RUN","PASS","FAIL","BLOCKED"};
  fprintf(r->out,"%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%u}",r->count++?",\n":"",id,names[status],rc,r->phase,r->nonce);
  fflush(r->out);
}
int h2_storage_mobile_phase(h2_runtime_config_t config,unsigned phase,uint32_t nonce,
    const char *path,int (*teardown)(void *),void *owner) {
  FILE *file=fopen(path,"w");if (!file) return H2_PAL_ERR_IO;
  fprintf(file,"{\"contract\":1,\"phase\":%u,\"nonce\":%u,\"pid\":%ld,\"cases\":[\n",phase,nonce,(long)getpid());
  reporter_t reporter={file,phase,nonce,0};
  h2_runtime_t *runtime=NULL;
  int rc=h2_runtime_init(&config,&runtime),cleanup=H2_PAL_ERR_INVALID_STATE;
  h2_pal_storage_result_t result={0};
  if (rc==H2_PAL_OK) {
    const h2_pal_storage_config_t tests={.root="/storage/run",.namespace_a="h2storea",.namespace_b="h2storeb",.nonce=nonce,.phase=(h2_pal_storage_phase_t)phase,.case_result=record,.user=&reporter};
    rc=h2_pal_storage_e2e_run(runtime,&tests,&result);
    if (!result.retained_cleanup) {
      h2_runtime_deinit(runtime);
      cleanup=teardown(owner);
    }
  } else cleanup=teardown(owner);
  fprintf(file,"\n],\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"cleanup\":%d,\"teardown\":%d,\"rc\":%d}\n",result.passed,result.failed,result.blocked,result.cleanup_result,cleanup,rc);
  if (fclose(file)!=0) return H2_PAL_ERR_IO;
  return rc==0 && cleanup==0?0:H2_PAL_ERR_INVALID_STATE;
}
