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
#define H2_PAL_OK 0
#define H2_PAL_ERR_UNAVAILABLE -2
#define H2_PAL_LOG_ERROR 3
#define MBEDTLS_X509_BADCERT_EXPIRED 0x01u
#define MBEDTLS_X509_BADCERT_OTHER 0x0100u
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200u
#define ESP_LOGE(...) ((void)0)
typedef struct mbedtls_x509_time { int year,mon,day,hour,min,sec; } mbedtls_x509_time;
typedef struct mbedtls_x509_crt {
    int version;
    mbedtls_x509_time valid_from,valid_to;
} mbedtls_x509_crt;
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


class CertificateTime(unittest.TestCase):
    def test_esp_and_bk_production_callbacks(self):
        compiler = shutil.which('cc')
        self.assertTrue(compiler)
        for backend, relative in [
            ('esp', 'native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c'),
            ('bk', 'native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c'),
        ]:
            source = (ROOT / relative).read_text()
            self.assertIn('mbedtls_ssl_conf_verify(&slot->config,', source)
            self.assertIn(backend+'_net_tls_verify_dates, NULL', source)
            start = source.index('static int '+backend+'_net_tls_verify_dates(')
            end = source.index('static h2_pal_result_t '+backend+'_net_tls_handshake(', start)
            prelude = PRELUDE.replace('TIME_API', 'h2_'+backend+'_platform_time_api')
            if backend == 'esp':
                prelude = prelude[:prelude.index('static const void *h2_bk_platform_log_api')]
            main = MAIN.replace('CHECK', backend+'_net_tls_verify_dates')
            with tempfile.TemporaryDirectory(prefix='h2-net-tls-cert-time-') as temporary:
                source_file = Path(temporary) / 'test.c'
                executable = Path(temporary) / 'test'
                source_file.write_text(prelude+source[start:end]+main)
                subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(source_file), '-o', str(executable)], check=True, timeout=30)
                subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
