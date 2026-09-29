"""Require source-bound, complete six-platform Audio Decoder qualification."""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path('projects/e2e/apps/pal-audio-decoder')
PLATFORMS = {'macos', 'wasm_aac_chrome', 'ios_simulator', 'android_emulator',
             'devkit_esp32s3', 'bk7258'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def ledger(report, ids):
    for key, value in dict(passed=len(ids), failed=0, blocked=0, retained=0, qualified=1, rc=0).items():
        assert report.get(key) == value, (key, report.get(key))
    assert report['frames'] > 0 and report['pcm_bytes'] > 0
    assert [c['id'] for c in report['cases']] == ids
    assert all(c['status'] == 'PASS' and c['detail'] == c['line'] == 0 for c in report['cases'])

q = json.loads((ROOT / 'qualification.json').read_text(encoding='utf-8'))
ids = re.findall(r'H2_PAL_ADEC_CASE\("([^"]+)"',
                 (ROOT / 'app/include/h2_pal_audio_decoder_cases.inc').read_text(encoding='utf-8'))
assert len(ids) == len(set(ids)) == 29
assert q['qualified'] is True and not q['pending']
assert set(q['platforms']) == PLATFORMS
assert q['interface'] == {'operations': 8, 'mandatory_cases': 29}
for path, sha in q['source_receipts'].items():
    assert digest(Path(path)) == sha, ('source changed', path)
for platform, entry in q['platforms'].items():
    assert entry['status'] == 'PASS', platform
    path = ROOT / entry['receipt']
    assert digest(path) == entry['receipt_sha256'], path
    r = json.loads(path.read_text(encoding='utf-8'))
    if platform in {'devkit_esp32s3', 'bk7258'}:
        assert set(r['independent_boots']) == {'install_boot', 'independent_reboot'}
        for boot in r['independent_boots'].values():
            ledger(dict(boot['report'], cases=boot['cases']), ids)
            assert boot['report']['operations'] == 8
        status = r['final_status']
        assert status['device_uid'] == r['device_uid']
        assert status['stage_valid'] == '0' and status['last_result'] == '0'
        assert status['running_partition'] == status['next_partition'] == '2'
        assert status['active_checksum'] == r['image_sha256']
        assert status['partition_2_package_checksum'] == r['package_sha256']
        assert status['partition_1_role'] == 'loader'
        if platform == 'bk7258':
            assert r['coredump_identity']['before_bytes'] == r['coredump_identity']['after_bytes'] == 32
            assert r['coredump_identity']['before_sha256'] == r['coredump_identity']['after_sha256']
        else:
            assert r['coredump_status']['blank'] == '1'
    else:
        ledger(r, ids)
        if platform != 'macos': assert r['teardown'] == 0
        if platform == 'wasm_aac_chrome':
            assert r['worker'] == 1 and r['exit_code'] == 0
            assert r['browser']['product'].startswith('Chrome/')
print('PAL_AUDIO_DECODER_QUALIFICATION PASS: six platforms, 29 cases, eight operations')
