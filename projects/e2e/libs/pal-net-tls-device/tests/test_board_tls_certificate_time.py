"""Compile the actual ESP/BK verification callbacks against controlled time."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]
PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#define MBEDTLS_PRIVATE(name) name
#define H2_PAL_OK 0
#define H2_PAL_ERR_UNAVAILABLE -2
#define H2_PAL_LOG_ERROR 3
#define MBEDTLS_X509_BADCERT_EXPIRED 0x01u
#define MBEDTLS_X509_BADCERT_OTHER 0x0100u
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200u
#define MBEDTLS_X509_BADCERT_NOT_TRUSTED 0x08u
#define ESP_LOGE(...) ((void)0)
typedef struct mbedtls_x509_time { int year,mon,day,hour,min,sec; } mbedtls_x509_time;
typedef struct mbedtls_x509_crt {
    int version;
    struct { unsigned char *p; size_t len; } raw;
    mbedtls_x509_time valid_from,valid_to;
} mbedtls_x509_crt;
typedef struct mbedtls_ssl_config {
    int (*f_vrfy)(void *, mbedtls_x509_crt *, int, uint32_t *);
    void *p_vrfy;
} mbedtls_ssl_config;
typedef struct esp_net_tls_socket {
    mbedtls_ssl_config config;
    int (*verify_chain)(void *, mbedtls_x509_crt *, int, uint32_t *);
    void *verify_chain_user;
} esp_net_tls_socket_t;
static void mbedtls_ssl_conf_verify(mbedtls_ssl_config *config,
    int (*callback)(void *,mbedtls_x509_crt *,int,uint32_t *), void *user){
 config->f_vrfy=callback;config->p_vrfy=user;
}
static uint64_t clock_ms;
static int clock_valid=1;
static const void *TIME_API(void){return &clock_ms;}
static int h2_pal_time_get_wall_ms(const void *api,uint64_t *out){(void)api;if(!clock_valid)return H2_PAL_ERR_UNAVAILABLE;*out=clock_ms;return H2_PAL_OK;}
static int mbedtls_x509_time_cmp(const mbedtls_x509_time *a,const mbedtls_x509_time *b){
 int aa[]={a->year,a->mon,a->day,a->hour,a->min,a->sec};
 int bb[]={b->year,b->mon,b->day,b->hour,b->min,b->sec};
 for(int i=0;i<6;i++){if(aa[i]!=bb[i])return aa[i]<bb[i]?-1:1;}
 return 0;
}
static const void *h2_bk_platform_log_api(void){return NULL;}
static int h2_pal_log_write(const void *api,int level,const char *scope,const char *message){
 (void)api;(void)level;(void)scope;(void)message;return H2_PAL_OK;
}
'''
MAIN = r'''
int main(void){
 mbedtls_x509_crt leaf={0};uint32_t flags=0;
 clock_ms=1790776800000ull; /* 2026-09-30 UTC */
 leaf.version=3;
 leaf.valid_from=(mbedtls_x509_time){2026,9,29,0,0,0};
 leaf.valid_to=(mbedtls_x509_time){2026,10,1,0,0,0};
 assert(CHECK(NULL,&leaf,0,&flags)==0&&flags==0);
 leaf.valid_to.year=2020;flags=0;
 assert(CHECK(NULL,&leaf,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_EXPIRED));
 leaf.valid_to.year=2026;leaf.valid_from.year=2030;flags=0;
 assert(CHECK(NULL,&leaf,1,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_FUTURE));
 leaf.valid_from.year=2026;clock_valid=0;flags=0;
 assert(CHECK(NULL,&leaf,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_OTHER));
 return 0;
}
'''
ESP_MAIN = r'''
static int verifier_calls, verifier_result;
static uint32_t verifier_flags;
static int trust_context;
static int verify_bundle(void *user,mbedtls_x509_crt *cert,int depth,uint32_t *flags){
 assert(user==&trust_context);(void)cert;(void)depth;
 ++verifier_calls;*flags=verifier_flags;return verifier_result;
}
int main(void){
 unsigned char der=1;
 mbedtls_x509_crt leaf={.version=3,.raw={&der,1}};uint32_t flags=0;
 clock_ms=1790776800000ull; /* 2026-09-30 UTC */
 leaf.valid_from=(mbedtls_x509_time){2026,9,29,0,0,0};
 leaf.valid_to=(mbedtls_x509_time){2026,10,1,0,0,0};
 esp_net_tls_socket_t socket={0};
 socket.config.f_vrfy=verify_bundle;socket.config.p_vrfy=&trust_context;
 esp_net_tls_install_verify_dates(&socket);
 assert(socket.config.f_vrfy==esp_net_tls_verify_dates&&socket.config.p_vrfy==&socket);
 assert(socket.config.f_vrfy(socket.config.p_vrfy,&leaf,0,&flags)==0&&flags==0);
 assert(verifier_calls==1);
 /* Real dates remain enforced even if the underlying verifier clears flags. */
 leaf.valid_to.year=2020;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&leaf,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_EXPIRED));
 leaf.valid_to.year=2026;leaf.valid_from.year=2030;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&leaf,1,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_FUTURE));
 leaf.valid_from.year=2026;clock_valid=0;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&leaf,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_OTHER));
 clock_valid=1;verifier_flags=MBEDTLS_X509_BADCERT_NOT_TRUSTED;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&leaf,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED));
 verifier_result=-9;
 assert(esp_net_tls_verify_dates(&socket,&leaf,0,&flags)==-9);
 verifier_result=0;verifier_flags=0;
 /* Bundle synthetic roots have no DER or date window. */
 mbedtls_x509_crt root={0};flags=0;
 assert(esp_net_tls_verify_dates(&socket,&root,2,&flags)==0&&flags==0);
 verifier_flags=MBEDTLS_X509_BADCERT_NOT_TRUSTED;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&root,2,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED));
 verifier_flags=0;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&root,0,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_EXPIRED));
 root.raw=leaf.raw;flags=0;
 assert(esp_net_tls_verify_dates(&socket,&root,2,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_EXPIRED));
 /* An explicit CA config has no bundle callback and never takes that exception. */
 root.raw.p=NULL;root.raw.len=0;esp_net_tls_socket_t explicit_ca={0};
 esp_net_tls_install_verify_dates(&explicit_ca);flags=0;
 assert(esp_net_tls_verify_dates(&explicit_ca,&root,2,&flags)==0&&(flags&MBEDTLS_X509_BADCERT_EXPIRED));
 return 0;
}
'''


class CertificateTime(unittest.TestCase):
    def test_esp_and_bk_production_callbacks(self):
        compiler = shutil.which('cc')
        self.assertTrue(compiler)
        for backend, relative in [
            ('esp', 'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c'),
            ('bk', 'native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c'),
        ]:
            source = (ROOT / relative).read_text()
            if backend == 'esp':
                self.assertIn('esp_net_tls_install_verify_dates(slot);', source)
            else:
                self.assertIn('mbedtls_ssl_conf_verify(&slot->config,', source)
                self.assertIn(backend+'_net_tls_verify_dates, NULL', source)
            start = source.index('static int '+backend+'_net_tls_verify_dates(')
            end = source.index('static h2_pal_result_t '+backend+'_net_tls_handshake(', start)
            prelude = PRELUDE.replace('TIME_API', 'h2_'+backend+'_platform_time_api')
            if backend == 'esp':
                prelude = prelude[:prelude.index('static const void *h2_bk_platform_log_api')]
            else:
                first = prelude.index('typedef struct mbedtls_ssl_config')
                last = prelude.index('static uint64_t clock_ms;')
                prelude = prelude[:first] + prelude[last:]
            main = ESP_MAIN if backend == 'esp' else MAIN.replace('CHECK', backend+'_net_tls_verify_dates')
            with tempfile.TemporaryDirectory(prefix='h2-net-tls-cert-time-') as temporary:
                source_file = Path(temporary) / 'test.c'
                executable = Path(temporary) / 'test'
                source_file.write_text(prelude+source[start:end]+main)
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(source_file), '-o', str(executable)], check=True, timeout=30)
                subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
