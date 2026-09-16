"""Exercise async resolver ownership with controlled lwIP callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class AsyncDnsTest(unittest.TestCase):
    def test_pending_close_capacity_and_completion(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_net.c").read_text()
        state = source[source.index("struct h2_pal_net_resolver {"):
                       source.index("static h2_pal_result_t map_socket_error")]
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
        main = r'''
int main(void) {
 h2_pal_net_resolver_t *r=NULL; h2_pal_net_addr_t out={0};
 char host[]="example.test";
 dns_result=ERR_INPROGRESS;
 assert(resolve_start(NULL,host,&r)==0 && queued_count==1);
 host[0]='X'; /* caller storage is not borrowed */
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_WOULD_BLOCK);
 assert(resolve_poll(NULL,r,&out,9)==H2_PAL_ERR_TIMEOUT && delays==0);
 assert(resolve_poll(NULL,r,&out,20)==H2_PAL_ERR_TIMEOUT && delays==2);
 resolve_close(NULL,r); assert(resolver_count==1);
 dispatch(); found("example.test",NULL,found_user); assert(resolver_count==0);
 h2_pal_net_resolver_t *slots[4]; dns_result=ERR_OK;
 for(int i=0;i<4;++i) assert(resolve_start(NULL,"example.test",&slots[i])==0);
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_NO_SPACE && !r);
 dispatch();
 for(int i=0;i<4;++i) {
  assert(resolve_poll(NULL,slots[i],&out,0)==0 && out.family==4 && out.port==0);
  assert(out.ip[0]==127 && out.ip[3]==1); resolve_close(NULL,slots[i]);
 }
 assert(resolver_count==0);
 queue_error=1;
 assert(resolve_start(NULL,"example.test",&r)==H2_PAL_ERR_NO_SPACE && !r);
 assert(resolver_count==0); queue_error=0; dns_result=ERR_MEM;
 assert(resolve_start(NULL,"example.test",&r)==0); dispatch();
 assert(resolve_poll(NULL,r,&out,0)==H2_PAL_ERR_NO_SPACE); resolve_close(NULL,r);
 dns_result=ERR_INPROGRESS;
 assert(resolve_start(NULL,"example.test",&r)==0); dispatch();
 ip_addr_t addr={0x0100007f}; found("example.test",&addr,found_user);
 assert(resolve_poll(NULL,r,&out,0)==0); resolve_close(NULL,r);
 assert(resolver_count==0);
 assert(resolve_start(NULL,"",&r)==H2_PAL_ERR_INVALID_ARG);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-dns-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + state + funcs + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
