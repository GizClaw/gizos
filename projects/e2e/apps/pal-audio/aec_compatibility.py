"""Project only audited optional diagnostics additions onto old Audio receipts.

Historical runs covered eleven ordinary Audio operations. Adding an explicit
UNSUPPORTED twelfth operation grants no diagnostic or new physical qualification.
Every byte outside the fixed compatibility patch must still match those runs.
"""

import hashlib
import json
from pathlib import Path


SCOPE_DIGEST = "9cc4550ac57ba03c41209c1edd44975706cefc189b207f53aba2a3f137210a97"
PATHS = frozenset({
    "libs/pal/providers/ios/pal_core/src/h2_ios_audio.m",
    "libs/pal/providers/android/pal_core/src/h2_android_platform.c",
    "boards/bk7258_v3_202405/bk7258/ap/src/h2_bk7258_board_audio.c",
    "projects/e2e/apps/pal-audio/check_qualification.py",
})
RECORD = Path("projects/e2e/apps/pal-audio/aec_optional_compatibility.json")


def source_content(path, content):
    name = str(path)
    if name not in PATHS:
        return content
    encoded = RECORD.read_bytes()
    assert hashlib.sha256(encoded).hexdigest() == SCOPE_DIGEST, "changed optional-AEC audit"
    audit = json.JSONDecoder().decode(encoded.decode("utf-8"))
    assert audit["new_physical_run_claimed"] is False and set(audit["files"]) == PATHS
    record = audit["files"][name]
    expected = record["previous_sha256"]
    if hashlib.sha256(content).hexdigest() == expected:
        return content
    restored = content
    for change in record["changes"]:
        before, after = change["before_utf8"].encode(), change["after_utf8"].encode()
        assert after and restored.count(after) == 1, (name, "not the audited compatibility addition")
        restored = restored.replace(after, before, 1)
    assert hashlib.sha256(restored).hexdigest() == expected, (
        name, "ordinary Audio bytes changed outside optional diagnostics stubs")
    return restored
