"""Keep historical device receipts separate from new native Host assembly."""
import hashlib
import json
from pathlib import Path
import re

RECORD = Path(__file__).with_suffix(".json")

def qualified_source(path, expected):
    path = str(path)
    actual = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if actual == expected:
        return
    audit = json.JSONDecoder().decode(RECORD.read_text())
    assert audit["schema"] == 1 and audit["new_physical_run_claimed"] is False
    assert audit["current_native_qualification"] == "NOT_RUN"
    assert audit["base_commit"] == "efcd603f43d50ed8f0754bfd3f7a21b763664239"
    # This exception cannot cover cases, fixtures, providers or device runners.
    assert all(key in ['projects/e2e/apps/pal-audio/check_qualification.py', 'projects/e2e/apps/pal-audio/BUILD.bazel', 'projects/e2e/apps/pal-audio-decoder/check_qualification.py', 'projects/e2e/apps/pal-audio-decoder/BUILD.bazel', 'projects/e2e/apps/pal-display/check_qualification.py', 'projects/e2e/apps/pal-display/BUILD.bazel'] or key in ['projects/e2e/targets/h2loader_tar_zlib/pal-audio/tests/BUILD.bazel', 'projects/e2e/targets/h2loader_tar_zlib/pal-audio/tests/test_failure_recovery.c'] or re.fullmatch(r"projects/e2e/targets/h2loader_tar_zlib/pal-(?:audio|audio-decoder|display)/(?:amoled|devkit|bk7258_v3_202405)/(?:BUILD\.bazel|CMakeLists\.txt|(?:main|ap)/(?:main\.c|ap_main\.c|CMakeLists\.txt))", key)
               for key in audit["source_changes"])
    change = audit["source_changes"].get(path)
    assert change and change["previous_sha256"] == expected, ("unowned historical source", path)
    assert change["current_sha256"] == actual, ("unrecorded current assembly", path)
    if path.endswith(("/main.c", "/ap_main.c")):
        assert suite_source(Path(path).read_text()) == change["protected_suite_sha256"], ("test/fixture body changed", path)

import re,hashlib

def suite_source(content):
    content = content.replace('#include "h2_pal_e2e_application_host.h"\n', '')
    match = re.search(r'(?m)^static\s+(?:void|h2_pal_result_t)\s+run\s*\([^)]*\)\s*\{', content)
    assert match, 'native test entry missing'
    brace = content.index('{', match.start()); depth = 0; end = None
    for token in re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]', content[brace:]):
        if token.group() == '{': depth += 1
        if token.group() == '}':
            depth -= 1
            if depth == 0: end = brace + token.start(); break
    assert end is not None
    body = content[brace+1:end]
    body = re.sub(r'^\s*runtime\s*=\s*app_runtime\s*;', '', body)
    body = body.replace('(void)unused;', '(void)user;')
    body = re.sub(r'while\s*\(!h2_application_host_should_stop\(host\)\)', 'for (;;)', body)
    body = re.sub(r'return\s+h2_pal_e2e_application_host_finish\(host,\s*runtime,\s*rc\);\s*$', '', body)
    body = re.sub(r'\bh2_pal_result_t waited = h2_pal_e2e_application_host_wait\(host, runtime\);\s*return h2_pal_e2e_application_host_finish\(host, runtime, rc == H2_PAL_OK \? waited : rc\);', 'hold();', body)
    body = re.sub(r'return\s+rc\s*;', 'return;', body)
    tokens = re.findall(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\s+|[A-Za-z_]\w*|\d+\w*|.', content[:match.start()] + body)
    tokens = [t for t in tokens if not t.isspace() and not t.startswith(('/*','//'))]
    return hashlib.sha256('\n'.join(tokens).encode()).hexdigest()
