# Resource state AMOLED E2E — 2026-09-07

## Executed source and firmware

Base commit `777aa1f338a509466c680043ea55f6c9a01b0a43` plus the uncommitted Resource implementation and E2E consumer in the shared worktree. The source snapshot did not change during build/run; per-file hashes are stored with local artifacts. This record does not claim that the base commit alone contains the Resource suite.

- Board: AMOLED / ESP32-S3, device UID `30eda0ae0f86`.
- Endpoint: `edge-bj-01.e2e.gizclaw.com:9821`, backend `h2peer`, RuntimeProfile `default`.
- Suite: `resource`, dedicated temporary Peer; version `0.1.0-resource-e2e`.
- Package SHA-256: `a74bd2a2193ee8752c33115994e6bd208de73bd68720520532ad28128d0948f2`, 1,460,408 bytes.
- App SHA-256: `af93ec2abe716739d26d2122a7a2c199ad093d80e237d1c7ba8c07402bf999ff`, 2,146,000 bytes.

Built with `--config=esp32s3 --define=H2_GIZCLAW_E2E_RESOURCE_ONLY=1 --//tools/bazel:firmware_version=0.1.0-resource-e2e` and the AMOLED package target. Installed through H2Loader managed send, verified Stage identity, then rebooted with `upgrade --monitor`.

## Result: PASS

Four Resource kinds completed in one live case (4,752 ms to terminal):

- Contacts: refresh, create, update and delete; confirmed field values and absence after deletion. A copied pre-update snapshot remained unchanged after the new commit.
- Profile: refreshed and changed name/emoji; snapshots confirmed both fields.
- Points: refreshed transactions and independently validated balance validity/result. No next page was returned, so live append was **not exercised**.
- Groups: refreshed the complete list; no multi-page fixture was seeded.
- Each kind: initial invalid/stale state, fresh valid snapshots, close retaining a stale snapshot with unchanged data revision, rejection of execute after close, destroy clearing its handle.

Final summary:

```text
suite=resource selected=1 terminal=1 pass=1 fail=0 error=0 blocked=0 cancelled=0 first_failure_case=- first_failure_rc=0 cleanup_rc=0 retained_resources=0 complete=true exit_code=0
```

Observed one case terminal and repeated summary. After the run, App identity matched the package, `stage_valid=0 last_result=0`, Loader checksum was unchanged, and coredump was `stored_bytes=0 blank=1`. The monitor was stopped after terminal replay (host exit 130); firmware reported exit 0.

## Local verification and boundaries

35 relevant local Bazel targets passed: portable E2E/coverage, the Resource consumer fault-injection test, public API/header checks, Desktop suite options, Guides and AMOLED launcher state. Fault injection validates stale/mismatched snapshots, failed balance, mutation timeout, closed admission and retained cleanup after destroy BUSY; it is not live server evidence.

This is the Resource subset, not full 215-API acceptance. It does not cover forced live multi-page data, concurrent execution, in-flight close, or physical audio. The preceding **same-firmware Session Voice retry still failed with PTT timeout `rc=-6`**; Resource success does not resolve that failure. See [Session Voice record](../bugs/2026-09-07-session-amoled.md).

Raw UART/send/status/coredump logs, firmware metadata/package and source hashes are in the local ignored directory `build/validation/resource-amoled/`. The board is left running `0.1.0-resource-e2e`.
