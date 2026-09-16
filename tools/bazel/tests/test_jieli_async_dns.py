"""Exercise async resolver ownership with controlled lwIP callbacks."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class AsyncDnsTest(unittest.TestCase):
    def test_pending_close_capacity_and_completion(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_net.c").read_text()
        assert 'static uint32_t stack_gate;' in source, 'DNS requires stack lifecycle settlement'
        state = source[source.index("struct h2_pal_net_resolver {"):
                       source.index("static h2_pal_result_t map_socket_error")]
        state = state[:state.index("enum { SLOT_RECV")] + source[source.index("static void resolver_reap(void);" if "static void resolver_reap(void);" in source else "static void stack_lock("):source.index("static int slot_enter(")]
        funcs = source[source.index("static void resolver_release("):
                       source.index("static int get_host_addr(")]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int h2_pal_result_t;
typedef int err_t;
enum { H2_PAL_OK=0,H2_PAL_ERR_NOT_FOUND=-1,H2_PAL_ERR_NO_SPACE=-2,
 H2_PAL_ERR_IO=-3,H2_PAL_ERR_INVALID_ARG=-4,H2_PAL_ERR_NO_MEMORY=-5,
 H2_PAL_ERR_WOULD_BLOCK=-6,H2_PAL_ERR_TIMEOUT=-7,
 H2_PAL_ERR_UNAVAILABLE=-8,H2_PAL_ERR_BUSY=-9,
 H2_PAL_NET_FAMILY_IPV4=4,ERR_OK=0,ERR_INPROGRESS=-1,ERR_MEM=-2,
 LWIP_DNS_ADDRTYPE_IPV4=0,DNS_MAX_NAME_LENGTH=256 };
typedef struct { int family; unsigned port; uint8_t ip[4]; } h2_pal_net_addr_t;
typedef struct h2_pal_net_resolver h2_pal_net_resolver_t;
typedef struct { uint32_t addr; } ip_addr_t;
#define IP_IS_V4(p) ((p)!=NULL)
#define ip_2_ip4(p) (p)
static void (*queued[8])(void *); static void *contexts[8];
static void (*found)(const char *,const ip_addr_t *,void *);
static void *found_user; static int queued_count, queue_error, dns_result;
static unsigned now, delays;
static void *tracked, *reusable;
static unsigned tracked_frees;
static void *counting_malloc(size_t n) {
 if(reusable) {void *p=reusable; reusable=NULL; return p;}
 return malloc(n);
}
static void counting_free(void *p) {
 if(p==tracked) {assert(!reusable); ++tracked_frees; reusable=p; return;}
 free(p);
}
#define malloc counting_malloc
#define free counting_free
static uint32_t timer_get_ms(void) { return now; }
static void os_time_dly(unsigned ticks) { assert(ticks==1); now+=10; ++delays; }
static err_t tcpip_try_callback(void (*fn)(void *),void *arg) {
 if(queue_error) return ERR_MEM;
 queued[queued_count]=fn; contexts[queued_count++]=arg; return ERR_OK;
}
static err_t dns_gethostbyname_addrtype(const char *host,ip_addr_t *addr,
 void (*fn)(const char *,const ip_addr_t *,void *),void *arg,int type) {
 assert(strcmp(host,"example.test")==0 && type==LWIP_DNS_ADDRTYPE_IPV4);
 addr->addr=0x0100007f; found=fn; found_user=arg; return dns_result;
}
static void dispatch(void) {
 int count=queued_count; queued_count=0;
 for(int i=0;i<count;++i) queued[i](contexts[i]);
}
'''
        stub += r'''
static int h2_jieli_atomic_cas_u32(uint32_t *p,uint32_t *e,uint32_t v) {
 return __atomic_compare_exchange_n(p,e,v,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void h2_jieli_atomic_store_u32(uint32_t *p,uint32_t v) {
 __atomic_store_n(p,v,__ATOMIC_RELEASE);
}
static unsigned registered(void);
'''
        main = r'''
static unsigned registered(void) {
 unsigned n=0; for(unsigned i=0;i<H2_JIELI_DNS_CAPACITY;++i) n+=resolvers[i]!=NULL;
 return n;
}
int main(void) {
 h2_pal_net_resolver_t *r=NULL; h2_pal_net_addr_t out={0};
 char host[]="example.test";
 dns_result=ERR_INPROGRESS;
 assert(resolve_start(NULL,host,&r)==H2_PAL_ERR_UNAVAILABLE);
 h2_jieli_net_stack_started();
 assert(resolve_start(NULL,host,&r)==0 && queued_count==1);
 host[0]='X'; /* caller storage is not borrowed */
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_WOULD_BLOCK);
 assert(resolve_poll(NULL,r,&out,9)==H2_PAL_ERR_TIMEOUT && delays==0);
 assert(resolve_poll(NULL,r,&out,20)==H2_PAL_ERR_TIMEOUT && delays==2);
 resolve_close(NULL,r); assert(registered()==1);
 dispatch(); found("example.test",NULL,found_user); assert(registered()==0);
 h2_pal_net_resolver_t *slots[4]; dns_result=ERR_OK;
 for(int i=0;i<4;++i) assert(resolve_start(NULL,"example.test",&slots[i])==0);
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_NO_SPACE && !r);
 dispatch();
 for(int i=0;i<4;++i) {
  assert(resolve_poll(NULL,slots[i],&out,0)==0 && out.family==4 && out.port==0);
  assert(out.ip[0]==127 && out.ip[3]==1); resolve_close(NULL,slots[i]);
 }
 assert(registered()==0);
 queue_error=1;
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_NO_SPACE && !r);
 assert(registered()==0); queue_error=0; dns_result=ERR_MEM;
 assert(resolve_start(NULL,"example.test",&r)==0); dispatch();
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_NO_SPACE); resolve_close(NULL,r);
 dns_result=ERR_INPROGRESS;
 assert(resolve_start(NULL,"example.test",&r)==0); dispatch();
 ip_addr_t addr={0x0100007f}; found("example.test",&addr,found_user);
 assert(resolve_poll(NULL,r,&out,0)==0); resolve_close(NULL,r);
 assert(registered()==0);
 assert(resolve_start(NULL,"",&r)==H2_PAL_ERR_INVALID_ARG);
 assert(resolve_start(NULL,"example.test",&r)==0); dispatch();
 void *late=found_user; tracked=r;
 unsigned frees_before=tracked_frees;
 h2_jieli_net_stack_stopping(); h2_jieli_net_stack_stopped();
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_UNAVAILABLE);
 assert(registered()==0); resolve_close(NULL,r);
 assert(tracked_frees==frees_before);
 int queued_before=queued_count;
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_UNAVAILABLE);
 assert(queued_count==queued_before);
 found("example.test",&addr,late);
 assert(registered()==0);
 h2_jieli_net_stack_started();
 assert(tracked_frees==frees_before+1);
 assert(resolve_start(NULL,"example.test",&r)==0 && (void *)r==tracked);
 found("example.test",&addr,late);
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_WOULD_BLOCK);
 resolve_close(NULL,r);
 unsigned pending_frees=tracked_frees;
 h2_jieli_net_stack_stopping(); h2_jieli_net_stack_stopped();
 assert(tracked_frees==pending_frees);
 dispatch(); /* queued begin after stop must not dereference its old pointer */
 assert(registered()==0);
 h2_jieli_net_stack_started();
 assert(tracked_frees==pending_frees+1);
 for(int i=0;i<4;++i) assert(resolve_start(NULL,"example.test",&slots[i])==0);
 for(int i=0;i<4;++i) resolve_close(NULL,slots[i]);
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_NO_SPACE);
 dispatch(); found("example.test",NULL,found_user);
 assert(resolve_start(NULL,"example.test",&r)==0);
 resolve_close(NULL,r);
 h2_jieli_net_stack_stopping(); h2_jieli_net_stack_stopped();
 h2_jieli_net_stack_started();
#undef free
 free(reusable);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-dns-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + state + funcs + main)
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CC", "cc"), *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
