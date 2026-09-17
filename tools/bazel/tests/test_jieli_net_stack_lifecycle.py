"""Stack drain, generation isolation and reentrant synchronous DNS."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_jieli_net_concurrency import SOURCE, run

DNS = r'''
typedef int err_t;
typedef struct { uint32_t addr; } ip_addr_t;
#define ip_2_ip4(p) (p)
enum { ERR_OK=0, ERR_VAL=-1, ERR_ARG=-2, ERR_MEM=-3, NETCONN_DNS_IPV4=0 };
static int dns_error;
#define gethostbyname forbidden_gethostbyname
static int netconn_gethostbyname_addrtype(const char *host,ip_addr_t *out,int type) {
 assert(strcmp(host,"example.test")==0 && type==NETCONN_DNS_IPV4);
 ++io_calls; out->addr=htonl(0x7f000001); return dns_error;
}
'''

MAIN = r'''
static void unavailable(int stale) {
 uint8_t b=0; int fd; h2_pal_net_addr_t out;
 int before=io_calls;
 if(!stale) {
  assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==H2_PAL_ERR_UNAVAILABLE);
  assert(udp_open(NULL,H2_PAL_NET_FAMILY_IPV4,0,&fd,&out)==H2_PAL_ERR_UNAVAILABLE);
  assert(tcp_open_bound(NULL,H2_PAL_NET_FAMILY_IPV4,NULL,&fd)==H2_PAL_ERR_UNAVAILABLE);
  assert(resolve_addr(NULL,"example.test",&out)==H2_PAL_ERR_UNAVAILABLE);
 }
 assert(tcp_connect(NULL,1,&address,10)==H2_PAL_ERR_UNAVAILABLE);
 assert(tcp_send(NULL,1,&b,1)==H2_PAL_ERR_UNAVAILABLE);
 assert(tcp_send_timeout(NULL,1,&b,1,10)==H2_PAL_ERR_UNAVAILABLE);
 assert(tcp_recv(NULL,1,&b,1,10)==H2_PAL_ERR_UNAVAILABLE);
 assert(udp_sendto(NULL,1,&address,&b,1)==H2_PAL_ERR_UNAVAILABLE);
 assert(udp_recvfrom(NULL,1,&out,&b,1,10)==H2_PAL_ERR_UNAVAILABLE);
 assert(udp_join_multicast(NULL,1,&address)==H2_PAL_ERR_UNAVAILABLE);
 close_socket(NULL,1);
 assert(io_calls==before);
}
int main(void) {
 unavailable(0);
 h2_jieli_net_stack_started();
 assert(stack_generation==1);
 int fd; assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0);
 h2_pal_net_addr_t out;
 assert(resolve_addr(NULL,"example.test",&out)==0);
 assert(out.ip[0]==127 && out.ip[1]==0 && out.ip[2]==0 && out.ip[3]==1);
 dns_error=ERR_VAL; assert(resolve_addr(NULL,"example.test",&out)==H2_PAL_ERR_NOT_FOUND);
 dns_error=ERR_ARG; assert(resolve_addr(NULL,"example.test",&out)==H2_PAL_ERR_NOT_FOUND);
 dns_error=ERR_MEM; assert(resolve_addr(NULL,"example.test",&out)==H2_PAL_ERR_IO);
 pthread_t reader, stop;
 block_recv=1; pthread_create(&reader,NULL,receiver,NULL); wait_blocked();
 pthread_create(&stop,NULL,stopper,NULL);
 for(;;) {
  stack_lock(); int ready=stack_ready; stack_unlock();
  if(!ready) break;
  os_time_dly(1);
 }
 assert(!atomic_load(&stopped));
 unavailable(0);
 release_blocked(); pthread_join(reader,NULL); pthread_join(stop,NULL);
 assert(thread_result==1 && atomic_load(&stopped));
 unavailable(0);
 h2_jieli_net_stack_started(); assert(stack_generation==2);
 unavailable(1);
 assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0);
 block_recv=0; uint8_t b; assert(tcp_recv(NULL,fd,&b,1,10)==1);
 return 0;
}
'''


class NetStackLifecycleTest(unittest.TestCase):
    def test_stack_drain_and_dns_storage(self):
        source = SOURCE.read_text()
        begin = source.index('static int resolve_addr(')
        end = source.index('static void resolver_release(', begin)
        resolver = source[begin:end]
        assert 'gethostbyname(' not in resolver, 'resolve_addr must not borrow gethostbyname storage'
        run(MAIN, DNS + resolver, source)


if __name__ == '__main__':
    unittest.main()
