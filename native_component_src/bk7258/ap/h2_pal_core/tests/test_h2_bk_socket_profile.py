"""Compile the production socket opener with both lwIP IPv6 profiles."""
import pathlib
import subprocess
import tempfile
import unittest


class SocketProfileTest(unittest.TestCase):
    def test_profiles_and_option_failure(self):
        source = (pathlib.Path(__file__).resolve().parents[1] /
                  'src/h2_bk_platform_net.c').read_text()
        start = source.index('static int bk_net_open_socket(')
        end = source.index('\nstatic int bk_net_bind_interface', start)
        function = source[start:end]
        fixture = r'''
#include <assert.h>
#define AF_INET 2
#define AF_INET6 10
#define SOCK_DGRAM 2
#if LWIP_IPV6
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY 27
#endif
static int opens, options, closes, open_result=7, option_result;
static int socket(int family,int type,int protocol) {
 assert(family==AF_INET || family==AF_INET6);
 assert(type==SOCK_DGRAM && protocol==0);++opens;return open_result;
}
static int setsockopt(int fd,int protocol,int option,const void *value,unsigned size) {
 assert(fd==7 && protocol==41 && option==27 && size==sizeof(int));
 assert(*(const int*)value==1);++options;return option_result;
}
static void closesocket(int fd) { assert(fd==7);++closes; }
'''
        main = r'''
int main(void) {
 assert(bk_net_open_socket(AF_INET,SOCK_DGRAM)==7);
 assert(opens==1 && options==0 && closes==0);
#if LWIP_IPV6
 assert(bk_net_open_socket(AF_INET6,SOCK_DGRAM)==7);
 assert(options==1 && closes==0);
 option_result=-1;
 assert(bk_net_open_socket(AF_INET6,SOCK_DGRAM)==-1);
 assert(options==2 && closes==1);
 open_result=-1;
 assert(bk_net_open_socket(AF_INET6,SOCK_DGRAM)==-1);
 assert(options==2 && closes==1);
#endif
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            c = pathlib.Path(directory) / 'test.c'
            exe = pathlib.Path(directory) / 'test'
            c.write_text(fixture + function + main)
            for enabled in [0, 1]:
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-Wno-unused-variable',
                                f'-DLWIP_IPV6={enabled}', str(c), '-o', str(exe)],
                               check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
