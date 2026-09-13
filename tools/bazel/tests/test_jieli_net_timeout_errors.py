"""Fault-inject timeout configuration in the real socket provider functions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NetTimeoutErrorsTest(unittest.TestCase):
    def test_failed_timeout_does_not_enter_io(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_net.c').read_text()
        def section(begin, end):
            return source[source.index(begin):source.index(end, source.index(begin))]
        functions = section('static h2_pal_result_t map_socket_error(', 'static int resolve_addr(')
        functions += section('static int udp_recvfrom(', 'static int udp_join_multicast(')
        functions += section('static int tcp_send_timeout(', 'static h2_pal_result_t tls_wrap(')
        stub = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "h2/pal/net/h2_pal_net.h"
static int fail_option, io_calls, flags_seen;
static int fake_setsockopt(int fd,int level,int opt,const void *v,socklen_t n) {
 (void)fd; assert(level==SOL_SOCKET && (opt==SO_RCVTIMEO || opt==SO_SNDTIMEO));
 assert(n==sizeof(struct timeval)); assert(v!=NULL);
 if (fail_option) {errno=EIO; return -1;} return 0;
}
static int fake_recvfrom(int fd,void *p,size_t n,int f,struct sockaddr *a,socklen_t *l) {
 (void)fd;(void)p;(void)n;(void)l; ++io_calls; flags_seen=f;
 if(a) memset(a,0,sizeof(struct sockaddr_in)); return 1;
}
static int fake_recv(int fd,void *p,size_t n,int f) {return fake_recvfrom(fd,p,n,f,NULL,NULL);}
static int fake_send(int fd,const void *p,size_t n,int f) {return fake_recv(fd,(void *)p,n,f);}
#define setsockopt fake_setsockopt
#define recvfrom fake_recvfrom
#define recv fake_recv
#define send fake_send
'''
        main = r'''
int main(void) {
 uint8_t data=0; h2_pal_net_addr_t addr={.family=H2_PAL_NET_FAMILY_IPV4};
 struct sockaddr_in native;
 assert(addr_to_sockaddr(&addr,&native)==H2_PAL_OK);
 for(int i=0;i<2;++i) {
  uint32_t wait=i ? 100u:0u; fail_option=1; io_calls=0;
  memset(&addr,0xa5,sizeof(addr)); h2_pal_net_addr_t before=addr;
  assert(udp_recvfrom(NULL,1,&addr,&data,1,wait)==H2_PAL_ERR_IO);
  assert(memcmp(&addr,&before,sizeof(addr))==0 && io_calls==0);
  assert(tcp_recv(NULL,1,&data,1,wait)==H2_PAL_ERR_IO && io_calls==0);
  assert(tcp_send_timeout(NULL,1,&data,1,wait)==H2_PAL_ERR_IO && io_calls==0);
  fail_option=0;
  assert(udp_recvfrom(NULL,1,&addr,&data,1,wait)==1);
  assert(flags_seen==(wait ? 0:MSG_DONTWAIT));
  assert(tcp_recv(NULL,1,&data,1,wait)==1);
  assert(flags_seen==(wait ? 0:MSG_DONTWAIT));
  assert(tcp_send_timeout(NULL,1,&data,1,wait)==1);
  assert(flags_seen==(wait ? 0:MSG_DONTWAIT));
 }
 assert(tcp_send(NULL,1,&data,1)==1 && flags_seen==MSG_DONTWAIT);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(stub + functions + main)
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
