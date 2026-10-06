#include "h2_gizclaw_e2e_internal.h"
#include "h2_app_test_mem.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

enum { NORMAL, CREATE_FAIL, CANCEL_FAIL, CANCEL_IGNORED, COMMIT_AFTER_CANCEL,
       MISSING_ABORT, DIRTY_READY, RECOVERY_FAIL, CLOSE_FAIL, DESTROY_FAIL, BUDGET };
struct h2_gizclaw_session {
  h2_gizclaw_session_config_t config;
  h2_gizclaw_session_state_t state;
  bool cancelled;
};
static unsigned mode, begins;
static struct h2_gizclaw_session session;
static h2_app_test_mem_t allocator;
static bool replacement;
static int service;
void h2_gizclaw_e2e_evidence(const char *s, const char *stage, int rc) { assert(s && stage); (void)rc; }
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t t) { assert(f && t==90000); return mode != BUDGET; }
h2_pal_result_t h2_gizclaw_session_create(const h2_gizclaw_session_config_t *c,h2_gizclaw_session_t **out) {
  assert(!*out); if(mode==CREATE_FAIL)return H2_PAL_ERR_NO_MEMORY;
  session=(struct h2_gizclaw_session){.config=*c}; *out=&session; replacement=true; return 0;
}
static int refresh(void) {
  if(mode==RECOVERY_FAIL && begins==2)return H2_PAL_ERR_IO;
  ++begins;session.cancelled=false;session.state.catalog=H2_GIZCLAW_SESSION_PREPARING;
  int rc=session.config.catalog_sink(session.config.catalog_sink_user,H2_GIZCLAW_CATALOG_BEGIN,NULL,"profile","revision");
  h2_gizclaw_workflow_page_t page={0};
  if(!rc)rc=session.config.catalog_sink(session.config.catalog_sink_user,H2_GIZCLAW_CATALOG_PAGE,&page,"profile","revision");
  if(!rc && session.cancelled)rc=H2_PAL_ERR_CLOSED;
  if(!rc || mode==COMMIT_AFTER_CANCEL) {
    session.config.catalog_sink(session.config.catalog_sink_user,H2_GIZCLAW_CATALOG_COMMIT,NULL,"profile","revision");
    session.state.catalog=H2_GIZCLAW_SESSION_READY;
  } else {
    if(mode!=MISSING_ABORT)session.config.catalog_sink(session.config.catalog_sink_user,H2_GIZCLAW_CATALOG_ABORT,NULL,"profile","revision");
    session.state.catalog=mode==DIRTY_READY ? H2_GIZCLAW_SESSION_READY : H2_GIZCLAW_SESSION_FAILED;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_session_register(h2_gizclaw_session_t *s,const char *token,uint32_t t) {
  assert(s==&session && token && t==30000);session.state.registration=H2_GIZCLAW_SESSION_READY;return refresh();
}
h2_pal_result_t h2_gizclaw_session_refresh(h2_gizclaw_session_t *s,uint32_t t) { assert(s==&session && t==30000);return refresh(); }
h2_pal_result_t h2_gizclaw_session_cancel_pending(h2_gizclaw_session_t *s) {
  assert(s==&session);if(mode==CANCEL_FAIL)return H2_PAL_ERR_IO;
  if(mode!=CANCEL_IGNORED){session.cancelled=true;++session.state.generation;}return 0;
}
h2_pal_result_t h2_gizclaw_session_snapshot(h2_gizclaw_session_t *s,h2_gizclaw_session_state_t *out) { assert(s==&session);*out=s->state;return 0; }
h2_pal_result_t h2_gizclaw_session_close(h2_gizclaw_session_t *s) {
  assert(s==&session);return replacement && mode==CLOSE_FAIL ? H2_PAL_ERR_IO : 0;
}
h2_pal_result_t h2_gizclaw_session_destroy(h2_gizclaw_session_t **s) {
  if(replacement && mode==DESTROY_FAIL)return H2_PAL_ERR_IO;*s=NULL;return 0;
}
int main(void) {
  for(mode=NORMAL;mode<=BUDGET;++mode) {
    h2_app_test_mem_init(&allocator,NULL);begins=0;replacement=false;
    h2_runtime_t runtime={0};
    h2_gizclaw_e2e_fixture_t f={.runtime=&runtime,.allocator=&allocator.api,
       .registration_token="local-token",.runtime_profile_name="profile",
       .actors={{.service=(h2_gizclaw_service_t *)&service,.session=&session}}};
    int rc=h2_gizclaw_e2e_run_session_cancel(&f);
    assert((rc==0)==(mode==NORMAL));
    if(mode==CLOSE_FAIL || mode==DESTROY_FAIL) {
      assert(f.case_cleanup && f.case_state && f.actors[0].session);
      unsigned failed_mode=mode;mode=NORMAL;assert(f.case_cleanup(&f)==0);mode=failed_mode;
    }
    assert(!f.case_state && !f.case_cleanup && allocator.live_blocks==0);
  }
  return 0;
}
