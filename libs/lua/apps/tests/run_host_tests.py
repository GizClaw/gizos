#!/usr/bin/env python3
"""Compile the production Lua app manager and device handlers with host PAL.

Supply yyjson's source directory and a GizClaw source checkout with generated
C contracts and its pinned nanopb submodule. No board SDK or Bazel is needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--yyjson-dir', type=Path, required=True)
    dependency = parser.add_mutually_exclusive_group(required=True)
    dependency.add_argument('--gizclaw-dir', type=Path)
    dependency.add_argument('--sdk-dir', type=Path, help='Unpacked C SDK source module')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    if args.gizclaw_dir:
        sdk = args.gizclaw_dir.resolve() / 'sdk/c/gizclaw'
        nanopb = args.gizclaw_dir.resolve() / 'third_party/nanopb/upstream'
    else:
        sdk = args.sdk_dir.resolve()
        nanopb = sdk / 'third_party/nanopb'
    yyjson = args.yyjson_dir.resolve()
    for required in [yyjson / 'yyjson.c', nanopb / 'pb_decode.c', sdk / 'generated/payload/lua_app.pb.c']:
        if not required.is_file():
            parser.error(f'missing dependency source: {required}')
    includes = [repo / name for name in [
        'libs/testing/include', 'libs/sync/atomic/include', 'libs/archive/bundle/include',
        'libs/crypto/sha256/include', 'libs/encoding/include',
        'libs/clients/gizclaw/include', 'libs/lua/apps/include', 'pal/include',
        'pal/providers/platforms/desktop/pal_core/include', 'libs/semver/include',
        'libs/lua/include', 'libs/command/include',
        'libs/runtime/include', 'libs/net/wifi_sta/include',
        'pal/providers/backends/yyjson/include',
        'projects/testing/integration/gizclaw/app/include',
    ]]
    includes += [repo / 'libs/clients/gizclaw/src', sdk / 'include', sdk / 'generated', nanopb, yyjson]
    common = [
        'libs/lua/apps/src/h2_lua_apps.c',
        'libs/lua/apps/src/h2_lua_app_install.c',
        'libs/testing/src/pal/h2_app_test_fs.c',
        'libs/sync/atomic/providers/c11/src/h2_atomic_c11.c',
        'libs/crypto/sha256/src/h2_sha256.c',
        'libs/semver/src/h2_semver.c',
        'libs/encoding/src/h2_encoding.c',
        'libs/encoding/src/h2_encoding_tables.c',
        'libs/archive/bundle/src/h2_bundle_tar.c',
        'libs/archive/bundle/src/h2_bundle_path.c',
        'pal/providers/platforms/desktop/pal_core/src/h2_desktop_platform_core.cpp',
        'pal/providers/platforms/desktop/pal_core/src/h2_desktop_resource_stats.cpp',
    ]
    device = [
        repo / 'libs/clients/gizclaw/src/h2_gizclaw_lua_apps.c',
        repo / 'libs/clients/gizclaw/src/h2_gizclaw_lua_stream.c',
        sdk / 'src/gzc_common.c',
        sdk / 'src/gzc_buffer.c',
        sdk / 'src/gzc_platform.c',
        sdk / 'generated/payload/lua_app.pb.c',
        *[nanopb / f'pb_{name}.c' for name in ['common', 'decode', 'encode']],
    ]
    targets = {
        'apps': (repo / 'libs/lua/apps/tests/test_apps.c', []),
        'close_failure': (repo / 'libs/lua/apps/tests/test_close_failure.c', []),
        'device': (repo / 'libs/clients/gizclaw/tests/test_h2_gizclaw_lua_apps.c', device),
    }
    with tempfile.TemporaryDirectory(prefix='gizos-lua-host-') as directory:
        output = Path(directory)
        fixture = output / 'libs/lua/apps/fixtures.h'
        fixture.parent.mkdir(parents=True)
        subprocess.run([sys.executable, str(repo / 'libs/lua/apps/tests/fixtures.py'), '--output', str(fixture)], check=True)
        flags = ['-g', '-Wall', '-Wextra', '-Werror', '-pthread']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        flags += [f'-I{path}' for path in [*includes, output, fixture.parent]]
        objects = {}

        def compile_source(source):
            if source not in objects:
                cpp = source.suffix == '.cpp'
                compiler = os.environ.get('CXX' if cpp else 'CC', 'c++' if cpp else 'cc')
                obj = output / f'{len(objects)}.o'
                subprocess.run([compiler, '-std=c++17' if cpp else '-std=c11', *flags, '-c', str(source), '-o', str(obj)], check=True)
                objects[source] = obj
            return str(objects[source])

        shared = [compile_source(repo / name) for name in common]
        shared.append(compile_source(yyjson / 'yyjson.c'))
        for name, (test, sources) in targets.items():
            executable = output / name
            subprocess.run([os.environ.get('CXX', 'c++'), *flags, *shared,
                            *[compile_source(source) for source in [test, *sources]],
                            '-lz', '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=60)
            print(f'{name}: PASS', flush=True)
        for name in ['libs/clients/gizclaw/src/h2_gizclaw_client.c',
                     'libs/clients/gizclaw/src/h2_gizclaw_device.c',
                     'projects/testing/integration/gizclaw/app/src/h2_gizclaw_e2e_lua_apps.c']:
            compile_source(repo / name)
        print('SDK consumer and E2E compilation: PASS', flush=True)


if __name__ == '__main__':
    main()
