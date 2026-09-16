"""Fault-inject timeout configuration in the real socket provider functions."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NetTimeoutErrorsTest(unittest.TestCase):
    def test_failed_timeout_does_not_enter_io(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_net.c').read_text()
        def section(begin, end):
            return source[source.index(begin):source.index(end, source.index(begin))]
        functions = section('static uint32_t stack_gate;', 'static h2_pal_result_t map_socket_error(')
        functions += section('static h2_pal_result_t map_socket_error(', 'static int resolve_addr(')
        functions += section('static int udp_recvfrom_active(', 'static int udp_join_multicast_active(')
        functions += section('static int tcp_connect_active(', 'static h2_pal_result_t tls_wrap(')
        stub = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
 if(a) memset(a,0,sizeof(struct sockaddr_in));
 return 1;
}
static int fake_recv(int fd,void *p,size_t n,int f) {return fake_recvfrom(fd,p,n,f,NULL,NULL);}
static int fake_send(int fd,const void *p,size_t n,int f) {return fake_recv(fd,(void *)p,n,f);}
static int mode_calls, connect_pending, fail_restore;
static int initial_mode, current_mode, terminal_stage, fail_flags, fail_nonblocking;
int fake_fcntl(int fd, int cmd, int value) {
 (void)fd;
 (void)value;
 assert(cmd==F_GETFL);
 if(fail_flags) {errno=EIO; return -1;}
 return initial_mode ? O_NONBLOCK : 0;
}
#define fcntl fake_fcntl
static int fake_ioctl(int fd,unsigned long cmd,unsigned long *mode) {
 (void)fd;(void)cmd; ++mode_calls;
 current_mode=(*mode!=0);
 if(*mode!=0 && fail_nonblocking) {errno=EIO; return -1;}
 if (*mode==0 && fail_restore) {errno=EIO; return -1;} return 0;
}
static int fake_connect(int fd,const struct sockaddr *a,socklen_t len) {
 (void)fd;(void)a;(void)len;
 if (terminal_stage==1) {errno=ECONNREFUSED; return -1;}
 if (connect_pending) {errno=EINPROGRESS; return -1;} return 0;
}
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
 (void)n;(void)r;(void)w;(void)e;(void)t;
 if (terminal_stage==2) {errno=EIO; return -1;}
 if (terminal_stage==5) return 0;
 return 1;
}
static int fake_getsockopt(int fd,int l,int o,void *v,socklen_t *n) {
 (void)fd;(void)l;(void)o;(void)n;
 if (terminal_stage==3) {errno=EIO; return -1;}
 *(int *)v=terminal_stage==4 ? ECONNREFUSED : 0;
 return 0;
}
#define FIONBIO 1
#define ioctlsocket fake_ioctl
#define connect fake_connect
#define select fake_select
#define getsockopt fake_getsockopt
#define setsockopt fake_setsockopt
#define recvfrom fake_recvfrom
#define recv fake_recv
#define send fake_send
'''
        stub += r'''
#define MEMP_NUM_NETCONN 55
#define LWIP_SOCKET_OFFSET 0
static int h2_jieli_atomic_cas_u32(uint32_t *p,uint32_t *e,uint32_t v) {
 return __atomic_compare_exchange_n(p,e,v,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void h2_jieli_atomic_store_u32(uint32_t *p,uint32_t v) {
 __atomic_store_n(p,v,__ATOMIC_RELEASE);
}
static void os_time_dly(unsigned n) {(void)n;}
'''
        main = r'''
int main(void) {
 h2_jieli_net_stack_started();
 assert(stack_enter()==0); stack_leave(); slot_opened(1);
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
 addr.family=H2_PAL_NET_FAMILY_IPV4;
 for (connect_pending=0;connect_pending<=1;++connect_pending) {
  mode_calls=0; fail_restore=1;
  assert(tcp_connect(NULL,1,&addr,100)==H2_PAL_ERR_IO && mode_calls==2);
  mode_calls=0; fail_restore=0;
  assert(tcp_connect(NULL,1,&addr,100)==H2_PAL_OK && mode_calls==2);
 }
 fail_flags=1;
 assert(tcp_connect(NULL,1,&addr,100)==H2_PAL_ERR_IO && sockets[1].busy==0);
 fail_flags=0; fail_nonblocking=1;
 assert(tcp_connect(NULL,1,&addr,100)==H2_PAL_ERR_IO && sockets[1].busy==0);
 fail_nonblocking=0;
 connect_pending=1;
 for (initial_mode=0;initial_mode<=1;++initial_mode) {
  for (terminal_stage=1;terminal_stage<=4;++terminal_stage) {
   mode_calls=0;
   assert(tcp_connect(NULL,1,&addr,100)!=H2_PAL_OK);
   assert(mode_calls==2 && current_mode==initial_mode);
   assert(sockets[1].busy==0);
  }
 }
 terminal_stage=5;
 assert(tcp_connect(NULL,1,&addr,0)==H2_PAL_ERR_WOULD_BLOCK);
 assert(current_mode==1);
 assert(tcp_connect(NULL,1,&addr,100)==H2_PAL_ERR_TIMEOUT);
 assert(current_mode==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(stub + functions + main)
            binary = Path(directory) / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
