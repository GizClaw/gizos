"""Exercise the shipping STA persistence callback with real shared transaction logic."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[3]
class SaveTest(unittest.TestCase):
    def test_sdk_bool_header_order(self):
        source=(ROOT/'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c').read_text()
        headers = [line for line in source.splitlines()
                   if line in ('#include "asm/includes.h"', '#include "h2_wifi_sta.h"')]
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'asm').mkdir()
            (root/'asm/includes.h').write_text('typedef unsigned char bool;\n')
            (root/'test.c').write_text('\n'.join(headers)+'\nint main(void) { return 0; }\n')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-I',str(root),'-I',str(ROOT/'libs/pal/include'),
                            '-I',str(ROOT/'libs/wifi_sta/include'),str(root/'test.c'),
                            '-o',str(root/'test')],check=True)

    def test_save(self):
        source=(ROOT/'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c').read_text()
        shared=(ROOT/'libs/wifi_sta/tests/test_wifi_sta.c').read_text()
        fixture=shared[:shared.index('static void run_cases')]
        begin=source.find('static int sta_connect_and_save(void *user,')
        callback='' if begin<0 else source[begin:source.index('\n}\n',begin)+3]
        helper_begin=source.index('static int sta_validate_sdk_password(')
        helper=source[helper_begin:source.index('\n}\n',helper_begin)+3]
        wired='.connect_and_save = sta_connect_and_save' in source
        # Match the pinned SDK password storage used by the real preflight helper.
        fixture+=r'''
enum WIFI_MODE { STA_MODE=1, AP_MODE, P2P_MODE, SMP_CFG_MODE, MP_TEST_MODE, NONE_MODE };
enum P2P_ROLE { P2P_GC_MODE=1, P2P_GO_MODE };
struct wifi_store_info {
 enum WIFI_MODE mode;
 unsigned char pwd[2][64], ssid[2][33];
 enum P2P_ROLE p2p_role;
 unsigned char sta_cnt, connect_best_network;
} __attribute__((packed));
'''+helper+r'''
static fixture_t f;
static unsigned wifi_operation_busy;
static int wifi_operation_begin(void) { if(wifi_operation_busy) return H2_PAL_ERR_BUSY; wifi_operation_busy=1; return 0; }
static int sta_connect(void *u,const h2_pal_wifi_sta_config_t *c,uint32_t t) { return connect(u,c,t); }
static int sta_disconnect(void *u) { return disconnect(u); }
static int sta_get_status(void *u,h2_pal_wifi_sta_status_t *s) { return status(u,s); }
static const h2_pal_wifi_settings_api_t *h2_jieli_ac791n_devkit_wifi_settings_api(void) {
 static h2_pal_wifi_settings_api_t api; api=(h2_pal_wifi_settings_api_t){&f,&settings_vtable};return &api;
}
static const h2_pal_time_api_t *h2_jieli_wl82_platform_time_api(void) {
 static h2_pal_time_api_t api;api=(h2_pal_time_api_t){&f,&time_vtable};return &api;
}
'''+callback+r'''
int main(void) {
 (void)sta_vtable;(void)wifi_operation_begin;(void)sta_connect;(void)sta_disconnect;(void)sta_get_status;
 (void)h2_jieli_ac791n_devkit_wifi_settings_api;(void)h2_jieli_wl82_platform_time_api;
 const h2_pal_wifi_sta_config_t old={.ssid="old",.ssid_len=3};
 const h2_pal_wifi_sta_config_t target={.ssid="new",.ssid_len=3};
 const h2_pal_wifi_sta_vtable_t table={.connect_and_save=CALLBACK};
 const h2_pal_wifi_sta_api_t api={&f,&table};
 f=(fixture_t){.saved=old};
 assert(h2_pal_wifi_sta_connect_and_save(&api,&target,100)==0);
 assert(f.saves==1 && !memcmp(&f.saved,&target,sizeof(target)) && f.disconnects==1);
 f=(fixture_t){.saved=old,.connect_rc=H2_PAL_ERR_IO};
 assert(h2_pal_wifi_sta_connect_and_save(&api,&target,100)==H2_PAL_ERR_IO);
 assert(!f.saves && !memcmp(&f.saved,&old,sizeof(old)));
 f=(fixture_t){.saved=old,.save_rc=H2_PAL_ERR_IO};
 assert(h2_pal_wifi_sta_connect_and_save(&api,&target,100)==H2_PAL_ERR_IO);
 assert(f.saves==1 && !memcmp(&f.saved,&old,sizeof(old)));
 assert(!wifi_operation_busy);
 h2_pal_wifi_sta_config_t oversized=target;
 memset(oversized.password,'p',64); oversized.password_len=64;
 f=(fixture_t){.saved=old,.status={.state=H2_PAL_WIFI_STA_STATE_GOT_IP,.ip_valid=1}};
 assert(h2_pal_wifi_sta_connect_and_save(&api,&oversized,100)==H2_PAL_ERR_INVALID_ARG);
 assert(!f.disconnects && !f.connects && !f.saves && !wifi_operation_busy);
 assert(f.status.state==H2_PAL_WIFI_STA_STATE_GOT_IP && f.status.ip_valid);
 assert(!memcmp(&f.saved,&old,sizeof(old)));
 wifi_operation_busy=1;
 assert(h2_pal_wifi_sta_connect_and_save(&api,&target,100)==H2_PAL_ERR_BUSY);
 return 0;
}
'''.replace('CALLBACK','sta_connect_and_save' if wired else 'NULL')
        with tempfile.TemporaryDirectory() as d:
            unit=Path(d)/'test.c';binary=Path(d)/'test';unit.write_text(fixture)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'libs/pal/include'),'-I',str(ROOT/'libs/wifi_sta/include'),str(unit),str(ROOT/'libs/wifi_sta/src/h2_wifi_sta.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()
