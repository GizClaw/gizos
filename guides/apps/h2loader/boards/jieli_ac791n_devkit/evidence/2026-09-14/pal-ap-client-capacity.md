# AP client cache capacity

Base: `d3c0f4a1` (PR #178). SDK pin:
`eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, from
`tools/bazel/native_versions/jieli_ac791n_sdk_commit.txt`.
The local SDK HEAD matches the pin; the inspected archives, configuration source,
and public Wi-Fi header have no local modifications.

## SDK evidence

The pinned SDK has **five station-table slots**, WCID 0 through 4.
`apps/common/net/wifi_conf.c:389` describes `MAX_LEN_OF_MAC_TABLE(5)` as
the upper bound for `MaxStaNum`. That identifier is not exported by the SDK's
public headers. Independently, LLVM IR freshly decoded from `cmm_cfg.c.o` in
`cpu/wl82/liba/wl_wifi.a`, `wl_wifi_ap.a`, and `wl_wifi_sfc.a` shows:

```llvm
; wifi_get_sta_entry_rssi
%cmp = icmp slt i8 %wcid, 5
; The table access is into this array type:
[5 x %struct._MAC_TABLE_ENTRY]
```

SHA-256 of the archive members, in that order:

```text
63edd5e3d81ab0facd180847ffeea437b290cb8ece001941a04bffcb6c9ecab5
bcfdd7298f7641d219cd0f4ae7545f2e3509451bedad7fb8eaa0c2a3943dc38e
54a85d32ced0f3253afb78bd51af20d599558bc327a2fff526feb27dcb2f0dbe
```

Reproduce for each archive with the pinned JieLi toolchain's `llvm-ar p` to
extract `cmm_cfg.c.o` to a temporary `.bc` file, then
`clang -S -emit-llvm input.bc -o output.ll`.

`07e95f53` used a literal `8`, following
`apps/wifi_camera/wifi/wifi_app_task.c:60`: `for (int i = 0; i < 8; i++)`.
Both loops **break on a nonzero SDK result**. Eight is a conservative scan ceiling,
not evidence of eight available station slots. Starting an AP through the SDK
instead of PAL does not enlarge the compiled station table. Thus the reported
sixth-through-eighth-client loss is not reproducible within this pinned capacity.

## Change and regression

Name the owned cache bound `H2_JIELI_AP_STATION_SLOTS = 5`, with a conditional
static assertion against `MAX_LEN_OF_MAC_TABLE` if a future SDK exposes it.
Keep PAL `ap_start` validation, owned snapshots, and lock ownership unchanged.
An unexpected association when the cache is full now logs the uncached MAC and
capacity after releasing the gate. The count remains the number of owned entries;
this defensive diagnostic does not claim to track clients beyond the SDK bound.

The snapshots fixture associates five distinct MACs and checks every JOINED
payload, status count, full listing, duplicate suppression, truncated/zero-length
output, and each client's LEFT payload and leave/rejoin compaction. A synthetic
sixth association must emit exactly one diagnostic with its MAC. The diagnostic
also asserts that the PAL gate is not held.

Before changing the provider, the new test failed on `d3c0f4a1` under Apple Clang
21 and GCC 13.3 (`-Wall -Wextra -Werror`): expected one overflow diagnostic,
received zero. The five-client capacity checks passed on that revision; only the
missing diagnostic is a reproduced defect.

## Validation

- macOS: requested Bazel clients, snapshots, and operations targets all passed
  (3/3), using `--config=macos_arm64`.
- Linux VM `embed-zig-noble-amd64`: all three Python tests passed with GCC 13.3,
  `-Wall -Wextra -Werror`.
- Native Loader/display package build: both succeeded in 38.660 seconds
  (5 actions). The first attempt exposed a `stdio.h` / SDK `FILE` typedef
  conflict; removing that unnecessary include fixed it. Final-source host tests
  were rerun and passed.

Native command (no output-root owner was interrupted):

```sh
orb -m embed-zig-noble-amd64 bash -lc 'cd /Users/idy/GizClaw/gizos/.claude/worktrees/pr178-ap-cache && source /Users/idy/h2vivi/firmwares-devenv/export.sh && unset IDF_PATH H2LOADER_IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH && bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64- //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package'
```

No hardware operations were performed.
