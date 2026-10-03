#include "h2_gizclaw_e2e_social_ping.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { NORMAL, LOST, DUPLICATE, WRONG_SENDER, WRONG_GROUP, BAD_PAYLOAD,
       NOT_ONLINE, FALSE_DELIVERY, RATE_LIMIT, RATE_DELIVERED, CALL_ERROR, DEADLINE };
static unsigned mode, calls, fail_at, observed, assertion_count;
static bool emit, group_case, alive;
struct h2_gizclaw_req { bool done; } request;
static int step(void) { return ++calls == fail_at ? H2_PAL_ERR_IO : 0; }
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) { return (h2_gizclaw_str_t){s,strlen(s)}; }
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t t) {
  assert(f && t == 30000); return mode != DEADLINE;
}
void h2_gizclaw_e2e_evidence(const char *sym, const char *stage, int rc) {
  if (strstr(stage, "-assert") && !rc) ++assertion_count;
  if (emit) printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n",sym,stage,rc ? "FAIL" : "PASS",rc);
}
int h2_gizclaw_e2e_fixture_social_observation(h2_gizclaw_e2e_fixture_t *f,
    h2_gizclaw_e2e_actor_role_t role, h2_gizclaw_e2e_social_observation_t *out) {
  assert(f && role == (group_case ? H2_GIZCLAW_E2E_GROUP_MEMBER : H2_GIZCLAW_E2E_FRIEND));
  int rc = step(); if (rc) return rc;
  *out = (h2_gizclaw_e2e_social_observation_t){.count=observed,
      .invalid=mode == BAD_PAYLOAD && observed > 1u};
  strcpy(out->sender, mode == WRONG_SENDER ? "other" : "owner");
  if (group_case || mode == WRONG_GROUP) strcpy(out->group, mode == WRONG_GROUP ? "other" : "group");
  return 0;
}
static int result(h2_gizclaw_social_ping_t *out) {
  int rc = step(); if (rc) return rc;
  if (mode != LOST && mode != RATE_LIMIT && mode != NOT_ONLINE) observed += mode == DUPLICATE ? 2u : 1u;
  *out = (h2_gizclaw_social_ping_t){.result=H2_GIZCLAW_SOCIAL_PING_RESULT_DELIVERED, .delivered_count=1u};
  if (mode == FALSE_DELIVERY) out->delivered_count = 0;
  if (mode == NOT_ONLINE) { out->result=H2_GIZCLAW_SOCIAL_PING_RESULT_NOT_ONLINE; out->delivered_count=0; }
  if (mode == RATE_LIMIT || mode == RATE_DELIVERED) {
    out->result=H2_GIZCLAW_SOCIAL_PING_RESULT_RATE_LIMITED;
    out->delivered_count=0; out->has_retry_after_seconds=true; out->retry_after_seconds=5;
  }
  return 0;
}
static int create(h2_gizclaw_service_t *s, h2_gizclaw_req_t **out) {
  assert(s && !alive); *out=NULL; int rc=step(); if(rc) return rc;
  request.done=false; *out=&request; alive=true; return 0;
}
h2_pal_result_t h2_gizclaw_req_create_friend_ping(h2_gizclaw_service_t *s,uint64_t id,h2_gizclaw_str_t key,uint32_t t,h2_gizclaw_req_t **out) {
  assert(id==81 && key.len && t==30000); return create(s,out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_ping(h2_gizclaw_service_t *s,uint64_t id,h2_gizclaw_str_t key,uint32_t t,h2_gizclaw_req_t **out) {
  assert(id==80 && key.len && t==30000); return create(s,out);
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r, void *u,h2_gizclaw_req_input_read_fn in,h2_gizclaw_req_output_write_fn out,h2_gizclaw_req_complete_fn done) {
  assert(alive && r && !u && !in && !out && !done); return step();
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r,uint32_t t) { assert(alive && r && t==30000); int rc=step(); if(!rc)r->done=true; return rc; }
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) { assert(alive && r); return 0; }
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) { if(r) { assert(alive); alive=false; } }
h2_pal_result_t h2_gizclaw_resp_parse_friend_ping(const h2_gizclaw_req_t *r,h2_gizclaw_social_ping_t *o) { assert(alive && r->done); return result(o); }
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_ping(const h2_gizclaw_req_t *r,h2_gizclaw_social_ping_t *o) { assert(alive && r->done); return result(o); }
h2_pal_result_t h2_gizclaw_rpc_friend_ping(h2_gizclaw_service_t *s,h2_gizclaw_str_t key,uint32_t t,h2_gizclaw_social_ping_t *o) { assert(s && key.len && t==30000); return result(o); }
h2_pal_result_t h2_gizclaw_rpc_friend_group_ping(h2_gizclaw_service_t *s,h2_gizclaw_str_t key,uint32_t t,h2_gizclaw_social_ping_t *o) { assert(s && key.len && t==30000); return result(o); }
static int run(unsigned m, unsigned fail, bool group, bool req) {
  mode=m; fail_at=fail; calls=assertion_count=0; observed=1; alive=false; group_case=group;
  h2_gizclaw_e2e_fixture_t f={.friend_id="friend",.friend_group_name="group",
      .actors={{.service=(h2_gizclaw_service_t *)&request,.public_key="owner"}}};
  int rc=h2_gizclaw_e2e_check_social_ping(&f,group,req); assert(!alive); return rc;
}
int main(int argc,char **argv) {
  emit=argc==2 && !strcmp(argv[1],"--emit-success-evidence");
  if(emit)puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc");
  for(unsigned group=0;group<2;++group) {
    if(emit)printf("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/%s\n",group ? "group":"friend");
    for(unsigned req=0;req<2;++req) {
      assert(run(NORMAL,0,group,req)==0 && assertion_count==1);
      unsigned count=calls;
      if(!emit) {
        for(unsigned i=1;i<=count;++i)assert(run(CALL_ERROR,i,group,req)==H2_PAL_ERR_IO);
        for(unsigned m=LOST;m<=RATE_DELIVERED;++m) {
          int rc=run(m,0,group,req);
          assert((rc==0)==(m==RATE_LIMIT && !group && !req));
        }
        assert(run(DEADLINE,0,group,req)==H2_PAL_ERR_TIMEOUT);
      }
    }
    if(emit)printf("H2_GIZCLAW_E2E stage=coverage-end case=rpc/%s status=PASS rc=0 cleanup_rc=0\n",group ? "group":"friend");
  }
  if(emit)puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 cleanup_rc=0");
  return 0;
}
