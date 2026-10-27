# Mosaico GizClaw E2E

This board target reuses the GizClaw application and AMOLED launcher. It does
not introduce another PAL package. Case selection preserves the original
assertions; a subset run does not qualify the complete package.

The default selects all ten workloads. Board service addresses, registration credentials and trust
roots are explicit `app_config_fixture_*` build inputs. Keep private Bazel rc
files and generated artifacts outside the repository; use `--noannounce_rc`.

The workspace-fence case additionally requires `fence_workflow_name`,
`fence_first_id` and `fence_second_id` fixture flags. Provision the Workflow
binding and two distinct safety-fence identifiers in the same RuntimeProfile.
Empty defaults remain buildable; a launcher selecting this case rejects missing
configuration before networking. These inputs select test resources and do not
change PAL or GizClaw service interfaces.

The target enables ESP-IDF size optimization while retaining assertions. The
full image can still exceed the managed download filesystem's available space
with the default package compression. For a size-constrained test upload, a
standard zlib recompression of the **identical tar bytes** can fit. Verify the
decompressed tar, embedded application digest, manifest and package metadata
before upload, retain the original package and record the recompression proof.
Do not change the flash layout or remove cases to satisfy the staging limit.

Use the real GizClaw Server and its native Edge for HTTPS Device API ingress;
Server HTTP listeners intentionally reject public Device API routes. Pin the
Edge upstream key, allowlist its public key on Server and retain Device API
key authentication. Static download fixtures can use a separate read-only
HTTP/HTTPS server. Deterministic AI providers validate protocol behavior only;
their supported conversation and model capabilities must match each case.
