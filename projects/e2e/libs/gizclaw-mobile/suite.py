"""GizClaw's service fixture and business oracle; lifecycle is shared."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil

import api_coverage


def preflight(args):
    fixture = json.loads(args.fixtures["app_config"].read_text())
    if not all(fixture.get(key) for key in ("app_config_key", "runtime_profile", "app_config_value")):
        raise ValueError("explicit AppConfig key, expected value and RuntimeProfile required")
    fixture.update({key: os.environ.get(env, "") for key, env in (
        ("endpoint", "H2_GIZCLAW_E2E_ENDPOINT"),
        ("token", "H2_GIZCLAW_E2E_REGISTRATION_TOKEN"),
        ("api_url", "H2_GIZCLAW_E2E_DEVICE_API_URL"),
        ("audio_url", "H2_GIZCLAW_E2E_AUDIO_URL"))})
    endpoint = fixture["endpoint"]
    if not re.fullmatch(r"[a-zA-Z0-9.-]+:[0-9]{1,5}", endpoint) or not 0 < int(endpoint.rsplit(":", 1)[1]) <= 65535:
        raise ValueError("explicit hostname:port E2E endpoint required")
    if not fixture["token"] or len(fixture["token"]) > 4096 or not all(
            fixture[key].startswith("https://") for key in ("api_url", "audio_url")):
        raise ValueError("explicit E2E token and HTTPS device API/audio fixtures required")
    pcm = args.fixtures["pcm"].read_bytes()
    if not pcm or len(pcm) > 1024 * 1024 or len(pcm) % 2 or not any(pcm):
        raise ValueError("non-silent 16 kHz mono S16LE PCM required")
    api_coverage.validate_inventory(api_coverage.requirements(),
        (api_coverage.repository_root() / "libs/gizclaw/tests/public_api.inc").read_text())
    args.gizclaw_fixture = fixture


def _write_private(app, name, content):
    if app.platform == "ios":
        path = app.container / "Documents" / name
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.chmod(path, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(content)
    else:
        app.adb_command("shell", "run-as", app.package, "chmod", "600", "files/" + name)
        result = app.adb_command("shell", "run-as", app.package, "tee", "files/" + name,
                                 input=content, text=False, check=False)
        if result.returncode:
            # tee can echo secret input even on partial failure.
            raise RuntimeError("private E2E fixture write failed")


def _log(app):
    if app.platform == "ios":
        return (app.container / "Documents/gizclaw.log").read_text()
    return app.adb_command("shell", "run-as", app.package, "cat", "files/gizclaw.log", check=False).stdout


def run_suite(app, args):
    fixture = args.gizclaw_fixture
    shutil.copyfile(app.app, args.output / ("app.ipa" if app.platform == "ios" else "app.apk"))
    shutil.copyfile(app.sdk, args.output / "sdk.archive")
    args.gizclaw_leaked = False
    def redact(log):
        args.gizclaw_leaked |= fixture["token"] in log
        return re.sub(r"https?://[^\s)]+", "[redacted-url]",
                      log.replace(fixture["token"], "[REDACTED]"))
    try:
        with app.fixture("fixture.json", "{}"), app.fixture("voice.pcm", ""):
            _write_private(app, "fixture.json", json.dumps(fixture).encode())
            _write_private(app, "voice.pcm", args.fixtures["pcm"].read_bytes())
            report = app.launch(android_log="gizclaw.log")
        log = redact(_log(app))
        (args.output / "test.log").write_text(log)
        report["cases"] = [dict(id=case, status=status, rc=int(rc)) for case, status, rc in
            re.findall(r"H2_GIZCLAW_E2E stage=coverage-end case=([^ ]+) status=([^ ]+) rc=(-?\d+) ", log)
            if case in api_coverage.TOP_CASES]
        audit = api_coverage.audit(log.splitlines(keepends=True), api_coverage.requirements(),
            endpoint=fixture["endpoint"], backend="h2peer", profile=fixture["runtime_profile"],
            platform=args.report_platform, process_exit_code=report.get("rc", 1))
        (args.output / "coverage.json").write_text(json.dumps(audit, indent=2) + "\n")
        args.gizclaw_audit = audit
        app.environment().update(runtime_profile=fixture["runtime_profile"],
            app_config_key=fixture["app_config_key"],
            app_config_value_sha256=hashlib.sha256(fixture["app_config_value"].encode()).hexdigest(),
            fixture_contract_sha256=hashlib.sha256(args.fixtures["app_config"].read_bytes()).hexdigest(),
            pcm_sha256=hashlib.sha256(args.fixtures["pcm"].read_bytes()).hexdigest(),
            api_covered=audit["covered"], redaction_required=args.gizclaw_leaked)
        return report
    finally:
        for pattern in ("*.stdout", "*.stderr"):
            for path in args.output.glob(pattern):
                path.write_text(redact(path.read_text()))


def verify_report(_report, args):
    assert not args.gizclaw_leaked, "credential leaked into App output"
    assert args.gizclaw_audit["valid"], "GizClaw public API qualification incomplete"
