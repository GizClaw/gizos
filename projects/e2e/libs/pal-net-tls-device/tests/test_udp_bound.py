"""Compile actual board source-bind entry with deterministic syscall faults."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[5]
PREFIX=r'''
#include "h2/pal/net/h2_pal_net.h"
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
static int stage, created, closed, fallback;
static int family_to_lwip(h2_pal_net_family_t family) {return family==H2_PAL_NET_FAMILY_IPV4?AF_INET:AF_INET6;}
static int fake_socket(int family,int type,int protocol){(void)family;(void)type;(void)protocol;if(stage==1)return -1;++created;return 7;}
static int fake_setsockopt(int fd,int level,int key,const void *value,socklen_t length){(void)fd;(void)level;(void)key;(void)value;(void)length;return 0;}
static int fake_bind(int fd,const struct sockaddr *addr,socklen_t length){(void)length;assert(fd==7);const struct sockaddr_in *ip=(const void *)addr;assert(ntohs(ip->sin_port)==4242);assert(ntohl(ip->sin_addr.s_addr)==0xc0a80902u);return stage==3?-1:0;}
static int fake_getsockname(int fd,struct sockaddr *addr,socklen_t *length){(void)fd;if(stage==4)return -1;struct sockaddr_in value={.sin_family=AF_INET,.sin_port=htons(4242),.sin_addr={.s_addr=htonl(0xc0a80902u)}};memcpy(addr,&value,sizeof(value));*length=sizeof(value);return 0;}
static int fake_close(int fd){assert(fd==7);++closed;return 0;}
static int addr_to_sockaddr(const h2_pal_net_addr_t *addr,struct sockaddr_storage *out,socklen_t *length){if(stage==2)return H2_PAL_ERR_INVALID_ARG;struct sockaddr_in value={.sin_family=AF_INET,.sin_port=htons(addr->port)};memcpy(&value.sin_addr,addr->ip,4);memset(out,0,sizeof(*out));memcpy(out,&value,sizeof(value));*length=sizeof(value);return H2_PAL_OK;}
static int sockaddr_to_addr(const struct sockaddr *addr,h2_pal_net_addr_t *out){if(stage==5)return H2_PAL_ERR_FORMAT;const struct sockaddr_in *value=(const void *)addr;out->family=H2_PAL_NET_FAMILY_IPV4;out->port=ntohs(value->sin_port);memcpy(out->ip,&value->sin_addr,4);return H2_PAL_OK;}
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define getsockname fake_getsockname
#define close fake_close
#define closesocket fake_close
'''


class UDPBound(unittest.TestCase):
    def test_real_entries_clear_outputs_and_close_failed_socket(self):
        compiler=shutil.which('cc')
        self.assertTrue(compiler)
        for backend,path in [('esp','native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c'),('bk','native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c')]:
            source=(ROOT/path).read_text()
            self.assertIn('.udp_open_bound = '+backend+'_net_udp_open_bound',source)
            start=source.index('static int '+backend+'_net_udp_open_bound(')
            end=source.index('static int '+backend+'_net_udp_sendto(',start)
            entry=source[start:end]
            fallback=f'''static int {backend}_net_udp_open(void *user,h2_pal_net_family_t family,uint16_t port,int *out,h2_pal_net_addr_t *addr){{(void)user;(void)family;(void)port;(void)addr;++fallback;*out=11;return H2_PAL_OK;}}\n'''
            main=r'''
int main(void){
 h2_pal_net_bind_t source={.type=H2_PAL_NET_BIND_SOURCE_ADDR,.source_addr={.family=H2_PAL_NET_FAMILY_IPV4,.port=99,.ip={192,168,9,2}}};
 h2_pal_net_addr_t bound;int socket_fd;
 for(stage=0;stage<=5;++stage){created=closed=0;socket_fd=88;memset(&bound,0xa5,sizeof(bound));
 int result=ENTRY(NULL,H2_PAL_NET_FAMILY_IPV4,4242,&source,&socket_fd,&bound);
 if(!stage){assert(result==H2_PAL_OK&&socket_fd==7&&bound.port==4242&&memcmp(bound.ip,source.source_addr.ip,16)==0);assert(created==1&&closed==0);}
 else{assert(result!=H2_PAL_OK&&socket_fd==-1);assert(created==closed);}
 }
 source.type=H2_PAL_NET_BIND_NETIF;created=closed=0;socket_fd=88;
 assert(ENTRY(NULL,H2_PAL_NET_FAMILY_IPV4,4242,&source,&socket_fd,&bound)==H2_PAL_ERR_UNSUPPORTED&&socket_fd==-1&&created==0);
 source.type=H2_PAL_NET_BIND_SOURCE_ADDR;source.source_addr.family=H2_PAL_NET_FAMILY_IPV6;
 assert(ENTRY(NULL,H2_PAL_NET_FAMILY_IPV4,4242,&source,&socket_fd,&bound)==H2_PAL_ERR_INVALID_ARG&&created==0);
 assert(ENTRY(NULL,H2_PAL_NET_FAMILY_IPV4,4242,NULL,&socket_fd,&bound)==H2_PAL_OK&&fallback==1&&socket_fd==11);
 return 0;
}
'''.replace('ENTRY',backend+'_net_udp_open_bound')
            with tempfile.TemporaryDirectory(prefix='h2-net-udp-fault-') as temporary:
                test=Path(temporary)/'entry.c';binary=Path(temporary)/'entry'
                test.write_text(PREFIX+fallback+entry+main)
                subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'libs/pal/include'),str(test),'-o',str(binary)],check=True,timeout=30)
                subprocess.run([str(binary)],check=True,timeout=15)


if __name__=='__main__':unittest.main()
