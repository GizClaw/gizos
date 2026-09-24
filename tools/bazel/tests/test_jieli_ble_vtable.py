"""Compile the real board API factory and verify every PAL operation slot."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BOARD = 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c'
IMPLEMENTED = dict(zip(
    'start stop set_adv_data start_advertising stop_advertising adv_set_create adv_set_set_data adv_set_start adv_set_stop adv_set_destroy register_gatt_services unregister_gatt_services notify disconnect update_connection exchange_mtu set_preferred_phy'.split(),
    'h2_ble_start_retained h2_ble_stop h2_legacy_set_adv_data_retained h2_legacy_start_advertising_retained h2_legacy_stop_advertising_retained h2_adv_set_create_retained h2_adv_set_data_retained h2_adv_set_start_retained h2_adv_set_stop_retained h2_adv_set_destroy_retained h2_register_gatt_retained h2_unregister_gatt_retained h2_notify_retained h2_disconnect_retained h2_update_connection_retained h2_exchange_mtu_retained h2_set_phy_retained'.split()))
CALLS = {
    'adv_set_set_encoded_data': 'set, bytes, 2',
    'adv_set_set_scan_response_data': 'set, &adv',
    'start_scan': '&scan, scan_result, NULL',
    'stop_scan': '',
    'unregister_gatt_service': '&uuid',
    'indicate': '1, 1, bytes, 2, 100',
    'connect': '&addr, &connect, &handle',
    'configure_pairing': '&pairing',
    'pair': '1, 100',
    'read_phy': '1, &phy, 100',
    'gatt_discover': '1, &request, NULL, 0, &count, 100',
    'gatt_read': '1, 1, 0, bytes, sizeof(bytes), &count, 100',
    'gatt_write': '1, 1, bytes, sizeof(bytes), false, 100',
    'gatt_subscribe': '1, &subscribe, 100',
}


class BleVtableTest(unittest.TestCase):
    def test_complete_vtable(self):
        header = (ROOT / 'libs/pal/include/h2/pal/hal/h2_pal_ble.h').read_text()
        body = header.split('typedef struct h2_pal_ble_vtable {')[1].split('}')[0]
        operations = dict(re.findall(r'h2_pal_result_t \(\*(\w+)\)\((.*?)\);', body, re.S))
        self.assertEqual(set(operations), set(IMPLEMENTED) | set(CALLS))
        source = (ROOT / BOARD).read_text()
        start = source.index('const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(')
        factory = source[start:source.index('\n#else', start)]
        code = '#include <assert.h>\n#include "h2/pal/h2_pal_unsupported.h"\n'
        code += 'static int h2_ble_log_bind(const h2_pal_log_api_t *log) { (void)log; return 1; }\n'
        for operation, symbol in IMPLEMENTED.items():
            args = operations[operation]
            unused = ''.join('(void)' + re.search(r'(\w+)\s*$', p).group(1) + ';' for p in args.split(','))
            code += f'static h2_pal_result_t {symbol}({args}) {{ {unused} return 77; }}\n'
        code += factory
        code += '''
static bool scan_result(void *user, const h2_pal_ble_scan_result_t *result) {
    (void)user; (void)result; return false;
}
int main(void) {
    const h2_pal_ble_host_api_t *api = h2_jieli_ac791n_devkit_ble_host_api(NULL);
    const h2_pal_ble_vtable_t *v = api->vtable;
    unsigned char storage = 0;
    h2_pal_ble_adv_set_t *set = (h2_pal_ble_adv_set_t *)&storage;
    uint8_t bytes[2] = {1, 1};
    h2_pal_ble_adv_data_t adv = {0};
    h2_pal_ble_scan_params_t scan = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE, .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
        .interval_ms = 50, .window_ms = 50};
    h2_pal_ble_uuid_t uuid = {0};
    h2_pal_ble_addr_t addr = {0};
    h2_pal_ble_connect_params_t connect = {0};
    h2_pal_ble_pairing_config_t pairing = {0};
    h2_pal_ble_phy_info_t phy = {0};
    h2_pal_ble_gatt_discovery_request_t request = {
        .kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE, .start_handle = 1, .end_handle = 65535};
    h2_pal_ble_gatt_subscribe_t subscribe = {
        .value_handle = 1, .cccd_handle = 2, .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY, .enable = true};
    uint16_t handle = 1;
    size_t count = 0;
'''
        for operation in operations:
            code += f'assert(v->{operation} != NULL);\n'
        for operation, symbol in IMPLEMENTED.items():
            code += f'assert(v->{operation} == {symbol});\n'
        for operation, args in CALLS.items():
            code += f'assert(v->{operation}(api->user{", " if args else ""}{args}) == H2_PAL_ERR_UNSUPPORTED);\n'
            code += f'assert(v->{operation} == h2_pal_unsupported_ble_host_api()->vtable->{operation});\n'
            code += f'assert(h2_pal_ble_{operation}(api{", " if args else ""}{args}) == H2_PAL_ERR_UNSUPPORTED);\n'
            code += f'assert(h2_pal_ble_{operation}(NULL{", " if args else ""}{args}) == H2_PAL_ERR_INVALID_ARG);\n'

        # PAL wrappers retain validation precedence, including the new pairing slots.
        code += '''
assert(h2_pal_ble_configure_pairing(api, NULL) == H2_PAL_ERR_INVALID_ARG);
assert(h2_pal_ble_pair(api, 1, 0) == H2_PAL_ERR_INVALID_ARG);
assert(h2_pal_ble_connect(api, NULL, &connect, &handle) == H2_PAL_ERR_INVALID_ARG);
assert(h2_pal_ble_indicate(api, 1, 1, NULL, 1, 100) == H2_PAL_ERR_INVALID_ARG);
assert(h2_pal_ble_adv_set_set_encoded_data(api, set, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
assert(h2_pal_ble_unregister_gatt_service(api, NULL) == H2_PAL_ERR_INVALID_ARG);
return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'libs/pal/include/h2/pal'),
                str(unit), str(ROOT / 'libs/pal/src/unsupported/ble_host.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
