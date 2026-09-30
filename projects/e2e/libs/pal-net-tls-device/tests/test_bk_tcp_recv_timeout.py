"""Exercise BK production plain-socket receive with bounded syscall faults."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / 'native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c'
PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#include "h2/pal/net/h2_pal_net.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -0x7880
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
typedef struct bk_net_tls_socket { int ssl; } bk_net_tls_socket_t;
static uint64_t clock_ms;
static int mode, recv_calls, wait_calls, observed_flags;
static int bk_net_tls_acquire(int fd,int bounded,uint32_t ms,uint64_t deadline,bk_net_tls_socket_t **out,size_t *index){
 (void)fd;(void)bounded;(void)ms;(void)deadline;(void)out;(void)index;return 0;
}
static void bk_net_tls_release(size_t index){(void)index;}
static int mbedtls_ssl_read(int *ssl,uint8_t *data,size_t len){(void)ssl;(void)data;(void)len;return MBEDTLS_ERR_SSL_WANT_READ;}
static int bk_net_tls_wait(bk_net_tls_socket_t *socket,int rc,uint32_t ms){(void)socket;(void)rc;(void)ms;return H2_PAL_ERR_TIMEOUT;}
static uint64_t bk_net_now_ms(void){return clock_ms;}
static uint32_t bk_net_timeout_remaining_ms(uint64_t deadline){return clock_ms>=deadline?0u:(uint32_t)(deadline-clock_ms);}
static int wait_fd(int fd,int write_ready,uint32_t ms){
 (void)fd;(void)write_ready;++wait_calls;
 if(mode==1)return H2_PAL_OK;
 clock_ms+=ms;
 return H2_PAL_ERR_TIMEOUT;
}
static int bk_net_socket_error(void){return errno==ECONNRESET?H2_PAL_ERR_CLOSED:H2_PAL_ERR_IO;}
static int fake_recv(int fd,void *bytes,size_t len,int flags){
 (void)fd;assert(len==4);assert(flags==MSG_DONTWAIT);++recv_calls;observed_flags=flags;
 if(mode==1 && recv_calls==2){memcpy(bytes,"PONG",4);return 4;}
 if(mode==2)return 0;
 if(mode==3){errno=ECONNRESET;return -1;}
 errno=EWOULDBLOCK;return -1;
}
#define recv fake_recv
'''
MAIN = r'''
int main(void){
 uint8_t data[4]={0};
 mode=0;clock_ms=0;recv_calls=wait_calls=0;
 assert(bk_net_tcp_recv(NULL,7,data,4,0)==H2_PAL_ERR_WOULD_BLOCK);
 assert(recv_calls==1&&wait_calls==0&&observed_flags==MSG_DONTWAIT&&clock_ms==0);
 recv_calls=wait_calls=0;
 assert(bk_net_tcp_recv(NULL,7,data,4,40)==H2_PAL_ERR_TIMEOUT);
 assert(recv_calls==1&&wait_calls==1&&clock_ms==40);
 mode=1;recv_calls=wait_calls=0;
 assert(bk_net_tcp_recv(NULL,7,data,4,40)==4);
 assert(memcmp(data,"PONG",4)==0&&recv_calls==2&&wait_calls==1);
 mode=2;recv_calls=wait_calls=0;
 assert(bk_net_tcp_recv(NULL,7,data,4,0)==H2_PAL_ERR_CLOSED);
 mode=3;recv_calls=wait_calls=0;
 assert(bk_net_tcp_recv(NULL,7,data,4,0)==H2_PAL_ERR_CLOSED);
 return 0;
}
'''


class BKRecv(unittest.TestCase):
    def test_real_plain_socket_function(self):
        text = SOURCE.read_text()
        start = text.index('static int bk_net_tcp_recv(')
        end = text.index('static void bk_net_close(', start)
        source = PRELUDE + text[start:end] + MAIN
        compiler = shutil.which('cc')
        self.assertTrue(compiler)
        with tempfile.TemporaryDirectory(prefix='h2-bk-recv-fault-') as temporary:
            target = Path(temporary) / 'test.c'
            output = Path(temporary) / 'test'
            target.write_text(source)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'libs/pal/include'), str(target), '-o', str(output)],
                check=True, timeout=30)
            subprocess.run([str(output)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
