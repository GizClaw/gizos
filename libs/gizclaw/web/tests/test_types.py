"""Compile a consumer against only the actual exported SDK declaration tar."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('archive', 'node', 'compiler', 'consumer'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        with tarfile.open(args.archive) as archive:
            members = archive.getmembers()
            expected = {'LICENSE', 'h2_web_gizclaw.d.ts', 'method-map.d.ts', 'payload-codec.d.ts'}
            assert {member.name.lstrip('./') for member in members if member.isfile()} == expected
            for member in members:
                target = root / member.name
                assert target.resolve().is_relative_to(root.resolve())
                assert member.isfile() or member.isdir()
            archive.extractall(root)
        shutil.copyfile(args.consumer, root / 'consumer.ts')
        (root / 'tsconfig.json').write_text(json.dumps({
            'compilerOptions': {'strict': True, 'noEmit': True, 'target': 'ES2022',
                                'module': 'ESNext', 'moduleResolution': 'node',
                                'lib': ['ES2022', 'DOM'], 'types': []},
            'files': ['consumer.ts'],
        }))
        subprocess.run([str(args.node.resolve()), str(args.compiler.resolve()),
                        '-p', str(root / 'tsconfig.json')], check=True)
        print('GIZCLAW_WEB_TYPES consumer=PASS')


if __name__ == '__main__':
    main()
