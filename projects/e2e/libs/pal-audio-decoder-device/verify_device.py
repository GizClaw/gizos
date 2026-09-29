"""Validate one new boot ledger and the final managed App identity."""
import json
import re


def boot_ledger(text, registry, version):
    ids = re.findall(r'H2_PAL_ADEC_CASE\("([^"]+)"', registry)
    assert ids and len(ids) == len(set(ids)), "invalid case registry"
    # Reboot monitor can receive the old App's replay before the reboot. Only
    # consider the new BOOT marker and its first complete run, never old rows.
    marker = "H2_ADEC_BOOT version=" + version
    assert text.count(marker) == 1, "missing or repeated independent boot"
    current = text.split(marker, 1)[1]
    match = re.search(r'H2_ADEC_REPORT (\{[^\r\n]+\})', current)
    assert match, "missing terminal report"
    report = json.loads(match.group(1))
    cases = [json.loads(row) for row in re.findall(r'H2_ADEC_CASE (\{[^\r\n]+\})', current[:match.start()])]
    assert [case['id'] for case in cases] == ids, "incomplete or duplicate ledger"
    assert all(case['version'] == version and case['status'] == 'PASS' and
               case['detail'] == 0 and case['line'] == 0 for case in cases), "failed case"
    for key, value in dict(version=version, contract=1, operations=8, passed=len(ids),
                           failed=0, blocked=0, retained=0, qualified=1, rc=0).items():
        assert report.get(key) == value, (key, report)
    assert report['frames'] > 0 and report['pcm_bytes'] > 0, "no decoded PCM"
    assert 'H2_ADEC_READY rc=0 confirm=0' in current[match.end():], "App not confirmed"
    assert not re.search(r'panic|hard fault|assert failed|H2_ADEC_(?:LAUNCHER_FAIL|QUALIFICATION_FAIL|WATCHDOG)', current, re.I), "boot failure"
    return dict(report=report, cases=cases)


def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)', text))


def final_status(before, after, manifest, package_sha256, uid):
    initial, final = fields(before), fields(after)
    assert initial['device_uid'] == final['device_uid'] == uid, "device changed"
    for name in initial:
        if name.startswith('partition_1_'):
            assert final[name] == initial[name], ('Loader changed', name)
    expected = dict(active_role='app', running_partition='2', next_partition='2',
                    stage_valid='0', last_result='0', active_version=manifest['version'],
                    active_checksum=manifest['image_sha256'], partition_2_version=manifest['version'],
                    partition_2_image_checksum=manifest['image_sha256'],
                    partition_2_package_checksum=package_sha256)
    for key, value in expected.items():
        assert final.get(key) == value, (key, final)
    return final


def coredump_status(before, after):
    initial, final = fields(before), fields(after)
    for key in ('result', 'code', 'bytes', 'stored_bytes', 'blank'):
        assert initial[key] == final[key], ('coredump changed', key)
    assert final['result'] == 'OK' and final['code'] == '0'
    # A nonblank dump additionally needs before/after actual byte hashes.
    return final
