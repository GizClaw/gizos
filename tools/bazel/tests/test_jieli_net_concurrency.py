"""Threaded socket contract against extracted real provider functions."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_net.c'
STUB = r'''
#define _DEFAULT_SOURCE 1
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "h2/pal/net/h2_pal_net.h"
static int fail_option, io_calls, flags_seen, option_calls;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int blocked, release_io, close_io, block_recv, block_connect, block_send;
static int reuse_mode, reuse_entered, old_woken, reuse_release[2];
static int reuse_recv(void) {
 pthread_mutex_lock(&mu);
 int id=reuse_entered++;
 pthread_cond_broadcast(&cv);
 if(id<2) {
  while(!reuse_release[id]) pthread_cond_wait(&cv,&mu);
 }
 pthread_mutex_unlock(&mu);
 if(id==0) {errno=ENOTCONN; return -1;}
 return 1;
}
static void wait_io(void) {
 pthread_mutex_lock(&mu);
 assert(!blocked); blocked=1; pthread_cond_broadcast(&cv);
 while(!release_io) pthread_cond_wait(&cv,&mu);
 blocked=0; pthread_mutex_unlock(&mu);
}
static void resolver_reap(void) {}
static void os_time_dly(unsigned n) {
 (void)n; struct timespec t={0,1000000}; nanosleep(&t,NULL);
}
static int fake_setsockopt(int fd,int level,int opt,const void *v,socklen_t n) {
 ++option_calls; (void)fd; assert(level==SOL_SOCKET && (opt==SO_RCVTIMEO || opt==SO_SNDTIMEO));
 assert(n==sizeof(struct timeval)); assert(v!=NULL);
 if (fail_option) {errno=EIO; return -1;} return 0;
}
static int fake_recvfrom(int fd,void *p,size_t n,int f,struct sockaddr *a,socklen_t *l) {
 (void)fd;(void)p;(void)n;(void)l; ++io_calls; flags_seen=f;
 if(a) memset(a,0,sizeof(struct sockaddr_in));
 return 1;
}
static int fake_recv(int fd,void *p,size_t n,int f) {
 if(reuse_mode) return reuse_recv();
 if(block_recv) wait_io();
 if(close_io) {errno=ENOTCONN; return -1;}
 return fake_recvfrom(fd,p,n,f,NULL,NULL);
}
static int fake_send(int fd,const void *p,size_t n,int f) {if(block_send) wait_io(); return fake_recvfrom(fd,(void *)p,n,f,NULL,NULL);}
static int mode_calls, connect_pending, fail_restore;
static int initial_mode, current_mode, terminal_stage;
int fake_fcntl(int fd, int cmd, int value) {
 (void)fd;
 (void)value;
 assert(cmd==F_GETFL);
 return initial_mode ? O_NONBLOCK : 0;
}
#define fcntl fake_fcntl
static int fake_ioctl(int fd,unsigned long cmd,unsigned long *mode) {
 (void)fd;(void)cmd; ++mode_calls;
 current_mode=(*mode!=0);
 if (*mode==0 && fail_restore) {errno=EIO; return -1;} return 0;
}
static int fake_connect(int fd,const struct sockaddr *a,socklen_t len) {
 (void)fd;(void)a;(void)len;
 if (terminal_stage==1) {errno=ECONNREFUSED; return -1;}
 if (connect_pending) {errno=EINPROGRESS; return -1;} return 0;
}
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
 (void)n;(void)r;(void)w;(void)e;(void)t;
 if(block_connect) wait_io();
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

#define MEMP_NUM_NETCONN 55
#define LWIP_SOCKET_OFFSET 0
static int h2_jieli_atomic_cas_u32(uint32_t *p,uint32_t *e,uint32_t v) {
 return __atomic_compare_exchange_n(p,e,v,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void h2_jieli_atomic_store_u32(uint32_t *p,uint32_t v) {
 __atomic_store_n(p,v,__ATOMIC_RELEASE);
}

#define PP_HTONL(x) htonl(x)
static int fake_socket(int f,int t,int p) {(void)f;(void)t;(void)p; ++io_calls; return 1;}
static int fake_bind(int f,const struct sockaddr *a,socklen_t n) {(void)f;(void)a;(void)n; ++io_calls; return 0;}
static int fake_getsockname(int f,struct sockaddr *a,socklen_t *n) {(void)f;(void)n; memset(a,0,sizeof(struct sockaddr_in)); return 0;}
static int fake_close(int fd) {
 (void)fd; ++io_calls; pthread_mutex_lock(&mu);
 if(reuse_mode) {
  old_woken=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); return 0;
 }
 close_io=1; release_io=1;
 pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); return 0;
}
static int fake_sendto(int f,const void *p,size_t n,int flags,const struct sockaddr *a,socklen_t l) {
 (void)a;(void)l; return fake_send(f,p,n,flags);
}
#define socket fake_socket
#define bind fake_bind
#define getsockname fake_getsockname
#define closesocket fake_close
#define sendto fake_sendto
'''
HELPERS = r'''
static h2_pal_net_addr_t address={.family=H2_PAL_NET_FAMILY_IPV4};
static int thread_result;
static atomic_int stopped;
static void *receiver(void *p) {(void)p; uint8_t b; thread_result=tcp_recv(NULL,1,&b,1,100); return NULL;}
static void *connector(void *p) {(void)p; thread_result=tcp_connect(NULL,1,&address,100); return NULL;}
static void *sender(void *p) {(void)p; uint8_t b=0; thread_result=tcp_send_timeout(NULL,1,&b,1,100); return NULL;}
static void *stopper(void *p) {(void)p; h2_jieli_net_stack_stopping(); atomic_store(&stopped,1); return NULL;}
static void wait_blocked(void) {
 pthread_mutex_lock(&mu); while(!blocked) pthread_cond_wait(&cv,&mu); pthread_mutex_unlock(&mu);
}
static void release_blocked(void) {
 pthread_mutex_lock(&mu); release_io=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
}
'''
MAIN = r'''
int main(void) {
 (void)stopper;
 h2_jieli_net_stack_started(); int fd; uint8_t b=0;
 assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0);
 pthread_t t; block_recv=1; pthread_create(&t,NULL,receiver,NULL); wait_blocked();
 int options_before=option_calls;
 assert(tcp_recv(NULL,fd,&b,1,10)==H2_PAL_ERR_BUSY);
 assert(udp_recvfrom(NULL,fd,&address,&b,1,10)==H2_PAL_ERR_BUSY);
 assert(option_calls==options_before);
 assert(tcp_connect(NULL,fd,&address,10)==H2_PAL_ERR_BUSY);
 assert(tcp_send_timeout(NULL,fd,&b,1,10)==1);
 close_socket(NULL,fd); pthread_join(t,NULL);
 assert(thread_result==H2_PAL_ERR_CLOSED);
#ifdef HAS_STACK_GATE
 assert(sockets[fd-LWIP_SOCKET_OFFSET].busy==0 && stack_users==0);
#endif
 block_recv=0; close_io=0; release_io=0;
 assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0);
 block_connect=1; connect_pending=1; pthread_create(&t,NULL,connector,NULL); wait_blocked();
 assert(tcp_connect(NULL,fd,&address,10)==H2_PAL_ERR_BUSY);
 assert(tcp_recv(NULL,fd,&b,1,10)==H2_PAL_ERR_BUSY);
 assert(tcp_send(NULL,fd,&b,1)==H2_PAL_ERR_BUSY);
 release_blocked(); pthread_join(t,NULL); assert(thread_result==0);
 block_connect=0; connect_pending=0; release_io=0;
 block_send=1; pthread_create(&t,NULL,sender,NULL); wait_blocked();
 assert(tcp_send(NULL,fd,&b,1)==H2_PAL_ERR_BUSY);
 assert(udp_sendto(NULL,fd,&address,&b,1)==H2_PAL_ERR_BUSY);
 release_blocked(); pthread_join(t,NULL); block_send=0;
 fail_option=1; assert(tcp_recv(NULL,fd,&b,1,10)==H2_PAL_ERR_IO);
 fail_option=0; assert(tcp_recv(NULL,fd,&b,1,10)==1);
 for(terminal_stage=1;terminal_stage<=5;++terminal_stage) {
  connect_pending=1; assert(tcp_connect(NULL,fd,&address,10)<0);
  assert(tcp_send(NULL,fd,&b,1)==1);
 }
 terminal_stage=0; fail_restore=1; assert(tcp_connect(NULL,fd,&address,10)==H2_PAL_ERR_IO);
 fail_restore=0; assert(tcp_connect(NULL,fd,&address,10)==0);
 h2_jieli_net_stack_stopping(); h2_jieli_net_stack_started();
 assert(tcp_recv(NULL,fd,&b,1,10)==H2_PAL_ERR_UNAVAILABLE);
 return 0;
}
'''

REUSE_MAIN = r'''
static void *reuse_reader(void *p) {
 uint8_t b; *(int *)p=tcp_recv(NULL,1,&b,1,100); return NULL;
}
static void await_readers(int n) {
 pthread_mutex_lock(&mu);
 while(reuse_entered<n) pthread_cond_wait(&cv,&mu);
 pthread_mutex_unlock(&mu);
}
static void release_reader(int id) {
 pthread_mutex_lock(&mu); reuse_release[id]=1;
 pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
}
int main(void) {
 h2_jieli_net_stack_started(); reuse_mode=1;
 int fd, first_result, second_result; uint8_t b=0;
 assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0 && fd==1);
 pthread_t first, second;
 pthread_create(&first,NULL,reuse_reader,&first_result); await_readers(1);
 close_socket(NULL,fd);
 assert(old_woken); /* old recv is awake but parked before returning to PAL */
 assert(tcp_open(NULL,H2_PAL_NET_FAMILY_IPV4,&fd)==0 && fd==1);
 pthread_create(&second,NULL,reuse_reader,&second_result); await_readers(2);
 release_reader(0); pthread_join(first,NULL);
 assert(first_result==H2_PAL_ERR_CLOSED);
 assert(tcp_recv(NULL,fd,&b,1,10)==H2_PAL_ERR_BUSY);
 release_reader(1); pthread_join(second,NULL);
 assert(second_result==1);
 assert(tcp_recv(NULL,fd,&b,1,10)==1);
 return 0;
}
'''

def functions(source):
    def section(begin, end):
        return source[source.index(begin):source.index(end, source.index(begin))]
    gate = section('static uint32_t stack_gate;', 'static h2_pal_result_t map_socket_error(') if 'static uint32_t stack_gate;' in source else 'void h2_jieli_net_stack_started(void) {}\nvoid h2_jieli_net_stack_stopping(void) {}\n'
    return (gate + section('static h2_pal_result_t map_socket_error(', 'static int resolve_addr(')
            + section('static int bind_socket(', 'static h2_pal_result_t tls_wrap(')
            + section('static void close_socket(', 'const h2_pal_net_api_t *'))

def run(main, extra='', source=None):
    source = SOURCE.read_text() if source is None else source
    # Reference every extracted entry so -Werror also checks the whole provider slice.
    refs = '(void)resolver_reap; (void)tcp_send; (void)h2_jieli_atomic_cas_u32; (void)h2_jieli_atomic_store_u32; (void)os_time_dly; (void)udp_open; (void)tcp_open_bound; (void)udp_join_multicast; (void)udp_sendto; (void)udp_recvfrom; (void)close_socket; (void)connector; (void)sender; (void)receiver; (void)stopper; (void)wait_blocked; (void)release_blocked;'
    main = main.replace('int main(void) {', 'int main(void) {' + refs)
    with tempfile.TemporaryDirectory(prefix='h2-net-thread-') as directory:
        unit = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        unit.write_text(('#define HAS_STACK_GATE 1\n' if 'static uint32_t stack_gate;' in source else '') + STUB + functions(source) + extra + HELPERS + main)
        subprocess.run([os.environ.get('CC', 'cc'), *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(ROOT / 'libs/pal/include'), str(unit), '-o', str(binary)], check=True, timeout=60)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stdout + result.stderr

class NetConcurrencyTest(unittest.TestCase):
    def test_close_and_descriptor_reuse(self):
        run(REUSE_MAIN)

    def test_concurrent_operations(self):
        run(MAIN)

if __name__ == '__main__':
    unittest.main()
