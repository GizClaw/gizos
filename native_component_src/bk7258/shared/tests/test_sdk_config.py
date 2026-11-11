"""Check the production config adapters against both pinned SDK headers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=60, **kwargs)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--locator', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    sdk = Path(json.loads(args.locator.read_text())['paths']['checkout'])
    expected = (repo / 'tools/bazel/native/versions/bk7258_sdk_commit.txt').read_text().strip()
    assert run(['git', '-C', str(sdk), 'rev-parse', 'HEAD']).strip() == expected
    cc, cmake = shutil.which('cc'), shutil.which('cmake')
    assert cc and cmake, 'A host C compiler and CMake are required'
    shared = repo / 'native_component_src/bk7258/shared'
    overlay = shared / 'h2_bk_lwip_config.cmake'
    with tempfile.TemporaryDirectory(prefix='h2-bk-sdk-config-') as directory:
        work = Path(directory)
        includes = work / 'include'
        for relative, content in {
            'os/os.h': '', 'os/mem.h': '',
            'common/sys_config.h': '#include "sdkconfig.h"\n',
            'components/log.h': '', 'lwip/arch.h': '',
        }.items():
            path = includes / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (work / 'dummy.c').write_text('int config_probe;\n')
        (work / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(config_probe C)
add_library(lwip_probe STATIC dummy.c)
target_include_directories(lwip_probe PUBLIC
  "$ENV{{ARMINO_PATH}}/components/lwip_intf_v2_1/lwip-2.1.2/port")
function(armino_component_get_property output component property)
  set(${{output}} lwip_probe PARENT_SCOPE)
endfunction()
include("{overlay}")
get_target_property(paths lwip_probe INCLUDE_DIRECTORIES)
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/include_paths.txt" "${{paths}}")
''')

        def config(**options):
            (includes / 'sdkconfig.h').write_text('\n'.join(
                f'#define {name} {value}' for name, value in options.items())+'\n')

        def values(source, *extra):
            output = run([cc, '-E', '-P', '-x', 'c', '-I', str(includes),
                          '-I', str(shared), *extra, '-'], input=source)
            match = re.search(r'config_values\[\]\s*=\s*\{([^}]+)\}', output)
            assert match, output[-1000:]
            return [int(item.strip(), 0) for item in match.group(1).split(',')]

        fingerprints = {}
        for unit in ('ap', 'cp'):
            unit_sdk = sdk / unit
            tls = unit_sdk / 'components/psa_mbedtls/mbedtls_port/configs/mbedtls_psa_crypto_config.h'
            lwip = unit_sdk / 'components/lwip_intf_v2_1/lwip-2.1.2/port/lwipopts.h'
            fingerprints[unit] = (hashlib.sha256(tls.read_bytes()).hexdigest(),
                                  hashlib.sha256(lwip.read_bytes()).hexdigest())
            tls_source = f'''#include "{tls}"
#ifndef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#endif
int config_values[] = {{MBEDTLS_SSL_IN_CONTENT_LEN, MBEDTLS_SSL_OUT_CONTENT_LEN}};
'''
            user_config = '-DMBEDTLS_USER_CONFIG_FILE="h2_bk_mbedtls_record_config.h"'
            # Current baseline, a different explicit value, and absent Board options.
            for limits in ((16384, 4096), (8192, 2048), None):
                options = dict(CONFIG_FULL_MBEDTLS=0, CONFIG_SW_CRYPTO=1)
                if limits:
                    options.update(CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=limits[0],
                                   CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=limits[1])
                config(**options)
                assert values(tls_source, user_config) == list(limits or (16384, 8192))

            env = dict(os.environ, ARMINO_PATH=str(unit_sdk))
            build = work / unit
            run([cmake, '-S', str(work), '-B', str(build),
                 '-DCMAKE_C_COMPILER='+cc], env=env)
            generated = build / 'h2_bk_lwip_config'
            assert (build / 'include_paths.txt').read_text().split(';')[0] == str(generated)
            lwip_source = '''#include <lwipopts.h>
#ifndef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE 0
#endif
int config_values[] = {TCPIP_THREAD_STACKSIZE};
'''
            for stack in (1024, 2048):
                config(CONFIG_KEYVALUE=1, CONFIG_LWIP_TCPIP_THREAD_STACKSIZE=stack)
                assert values(lwip_source, '-I', str(generated)) == [stack]
            config(CONFIG_KEYVALUE=1)
            assert values(lwip_source, '-I', str(generated)) == [1024]
            config(CONFIG_FTP_SERVER=1)
            assert values(lwip_source, '-I', str(generated)) == ([1024] if unit == 'ap' else [0])
            assert fingerprints[unit] == (
                hashlib.sha256(tls.read_bytes()).hexdigest(),
                hashlib.sha256(lwip.read_bytes()).hexdigest())
            print(f'{unit}: TLS Board limits, SDK fallbacks and lwIP explicit/fallback values PASS')
        # Every known board consumer must declare the preserved effective baseline.
        for board in ('haivivi/h106_zero_bk_2_0', 'beken/bk7258_v3_202405'):
            for unit in ('ap', 'cp'):
                text = (repo / 'boards' / board / 'bk7258' / (unit+'.defaults')).read_text()
                for setting in ('CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384',
                                'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096',
                                'CONFIG_LWIP_TCPIP_THREAD_STACKSIZE=1024'):
                    assert setting in text, (board, unit, setting)
    print(json.dumps({'result': 'PASS', 'sdk_commit': expected}))


if __name__ == '__main__':
    main()
