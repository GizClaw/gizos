# Mosaico baseline and IDF 6.2 assessment — 2026-10-19

Baseline: main `d3ff30bc` (18 commits after `68e867d0`). The pre-rebase branch is preserved as `codex/mosaico-before-main-20261019-770113e8`. Rebase replays 78 branch commits; one module-lock refresh is already upstream. This assessment supersedes the previous **current** qualification status, while preserving every historical receipt in [the qualification ledger](esp_mosaico-qualification.md).

**Current result: rebase, architecture fixes and the selected build checks are complete; post-rebase hardware qualification remains open.** One initial host IPv6 UDP timeout remains unexplained despite three passing isolated repeats; full single-run host success is not claimed. The preceding Mosaico full-Flash restore and AMOLED original-App restore remain completed device-state records. This assessment does not install firmware or change those devices.

## Architecture decisions and resolved conflicts

- Keep main's direct App/Runtime composition and removal of App Host. Mosaico already composes the board providers, Runtime and managed App explicitly; no replacement Host framework is introduced.
- Keep main's removal of source/snapshot acceptance. The rebase retains Mosaico's actual coverage-log CLI test but drops the obsolete inventory-source comparison, which would call the deleted `validate_inventory`. Hash-bound historical firmware receipts establish identity, not execution by themselves.
- Combine the new ESP network address test with the S31 reboot tests, preserving the required Python test import. The other conflict is additive S31 toolchain documentation. There are four textual conflict stops across three files.
- Correct a semantic configuration-ownership mismatch: 16 Mosaico project-local SDK defaults move into board-owned `layouts/h2loader` or `layouts/board_e2e`, passed through the existing `layout_files`/`project_support_files` API. Repeated log settings share one layout file; existing SDK values and merge order are preserved. No named profile registry or new build-rule abstraction is added. Mosaico's canonical board defaults explicitly declare IPv6/SLAAC and three address slots, matching main's DevKit/AMOLED policy.
- Main's `app_version`/`AppVersionInfo` is compatible with existing firmware rules. The E2E command-line version setting remains an explicitly supported fallback; no duplicate board-owned release version is introduced. ESP's 31-byte descriptor check remains required.
- Keep the S31-specific toolchain/ABI, TinyUSB dual-CDC transport, safe-call ownership and private coroutine stack-guard adapter. Do not replace these with S3 Xtensa assumptions. Keep the shared IPv6, Runtime Wi-Fi serialization, Ogg Opus and bounded iperf cancellation changes from main, alongside the branch's SCTP timer/backpressure fixes.

## Mosaico support on this baseline

| Area | Implementation and qualification boundary |
| --- | --- |
| Board / SDK | Explicit ESP32-S31 target, pinned SDK `ddb3852ab5c7f9e6dbc268203ef9d612de391bf8`, GCC 16.1, Picolibc, PSRAM/XIP, revision-aware BSP wiring. Builds do not prove hardware behavior. |
| Runtime / management | Shared PAL/Runtime composition, board-owned USB transport, managed Loader/App partition layout and screen status. Historical 51-case Loader matrix passed; new-baseline recovery has not run. |
| Display / touch / controls / sensors | Existing board adapters and prior physical evidence remain available. Runtime lifecycle and SDK changes require a fresh integration smoke; BK DMA2D changes do not create a Mosaico DMA2D requirement. |
| Audio | Shared ES8311 capture/playback provider and Runtime forwarding. New `set_aec_observer` explicitly returns UNSUPPORTED; Mosaico still disables AEC. Ordinary Audio success does not establish acoustic echo-cancellation qualification. |
| Networking | Shared ESP dual-stack Net/Netif/Wi-Fi/TLS and H2Peer implementations are composed. IPv6/SLAAC is declared, but Mosaico has no `pal-ipv6` firmware target or 56-case hardware receipt yet. |
| iperf | Existing Mosaico 14-case TCP/UDP/SCTP entry remains a distinct contract. Main's `iperf-client` + touch `iperf-server/amoled` bench is new: Mosaico has no client launcher or three-mode IPv4/IPv6/dual-stack receipt. Its TCP/UDP results cannot replace SCTP coverage. |
| Camera | Optional board camera PAL, left SC101IOT 1280×720 UYVY with prior capture/preview/insertion evidence. No general Runtime camera service, encoder or network streaming qualification is implied. |
| Other hardware / business | NAND and unqualified board revisions/sensors remain outside existing physical coverage. GizClaw business acceptance remains excluded by the operator. |

## E2E work still required

Historical green results remain valid for their exact images, not for freshly rebased binaries. Prior R119/R120/R125, R106 and R108 are not relabelled as executions of `d3ff30bc` plus this branch.

| Priority | Required work | Reason / retained evidence |
| --- | --- | --- |
| P0 | Fresh Wi-Fi/Netif full contract, including current public IPv4/IPv6 readiness, disconnect/renewal, saved-settings preservation and fixture return | Main changes address lifecycle and Runtime settings concurrency. Historical R112 39/39 twice does not cover those changes. |
| P0 | Fresh Net/TLS, HTTP, MQTT, PAL-WebRTC and WebRTC performance | Resolver/address structures, fallback, scoped sockets, TLS/DTLS and ICE paths changed. Retain strict trust/name/fingerprint/timeout and cleanup checks. Historical R119 two smoke passes and R120 43/43 twice remain historical. Keep the original performance thresholds and verify a quiet host over each actual peer window. |
| P0 | Re-run the existing two-boot 14-case iperf matrix and real peer witnesses | Shared control cancellation, stream and address behavior changed. R125/R27 passed before this rebase; no transfer of acceptance. The old DevKit directional bottleneck remains an unresolved historical observation. |
| P0 | Integrate Mosaico with the shared `pal-ipv6` App/device helper; run its 56 mandatory cases with real IPv6 peers | New coverage includes strict TLS, DNS, HTTP/MQTT, DTLS and the full 43-case WebRTC registry. Preserve H2Peer board-peer vs Pion host-peer provenance. Missing target/evidence is an integration gap, not a passing optional case. |
| P1 | Integrate Mosaico with main's portable `iperf-client`; verify IPv4, IPv6 and dual-stack modes against the AMOLED touch server | New mode discovery, repeated measurements, public readiness, disconnect/reconnect, saved credentials and cancellation/UI lifecycle are absent from the old 14-case receipt. |
| P1 | Audio / Decoder and basic board interaction | Main adds AEC observer behavior, moves Ogg Opus parsing and reduces ESP internal-memory usage. Verify actual PCM, ordinary unsupported-AEC semantics, lifecycle/cleanup and decoder outputs; do not claim AEC acoustic performance. |
| P1 | Loader 51-case matrix, Core/Storage and Lua physical disconnect recovery | Shared network/Runtime code and SDK-dependent task/storage/USB/BLE behavior affect managed artifacts. Retain exact original image backup, cleanup, confirmation/rollback and original-layout restoration. |
| P2 | Remaining unchanged portable suites and board smoke | Atomic/JSON/Crypto/libco/Display retain earlier receipts. Run the new-image boot/cleanup coverage with the broad qualification batch; prioritize libco stack guards, PSRAM and actual hardware over repeating source-only checks. |

Net/TLS full-Net capability, IPv6 WAN/default-route changes and physical interface removal require their own declared profiles/environment. Neither core-profile nor local/AP-only IPv6 results establish them. AEC acoustic qualification additionally requires a supported real diagnostic provider/reference path and an independent near-end source. Those are capability/environment gaps, not thresholds to weaken.

## Why the IDF 6.2 upgrade also affects other ESP boards

Main still pins common ESP consumers to IDF 6.0 (`76f5dedd`, GCC 15.2). This branch intentionally pins **S3, P4 and C5 as well as S31** to IDF 6.2 (`ddb3852a`, GCC 16.1). The separate S31 locator does not isolate the common pin change. The previous October 11 build matrix and selected S3/Mosaico device runs are useful historical evidence, but do not qualify all current ESP boards.

| Target | Minimum additional evidence for the common upgrade |
| --- | --- |
| S3 DevKit / AMOLED | Native Loader and relevant E2E builds; Core/task/PSRAM, Storage, BLE/Lua reconnect, Wi-Fi dual-stack, Net/TLS/HTTP/MQTT, PAL-WebRTC/performance and Loader recovery. On AMOLED also Display/touch, Audio/Decoder and new touch-server/IPv6 fixture behavior. Current AMOLED iperf success alone does not cover the rest. |
| P4 | Reference plus real board-native builds; boot/XIP, RISC-V coprocessor and coroutine/task recovery, display/DMA, storage, remote Wi-Fi/Hosted transport and managed update/recovery on actual hardware. S31 RISC-V execution cannot substitute. |
| C5 | Reference plus actual consumer builds and physical boot, network/BLE, task/memory/storage and update behavior for the capabilities composed by that consumer. A reference smoke build is compile evidence only. |
| Non-ESP | IDF itself does not affect BK/JieLi/desktop/mobile/Web. Shared PAL ABI, Runtime and network changes still require their own compatible build/test CI; Mosaico board results do not qualify them. |

If common 6.2 validation is deferred, broad ESP qualification must remain open; it must not be inferred from successful S31 integration. This assessment does not undo the previously requested common SDK upgrade.

## Verification in this rebase

- Full compatible macOS host selection: **941 pass, 1 fail, 23 platform skips** out of 965 targets; 398 executed and the remainder reused valid cached results. The failure is `desktop_pal_ipv6_test`, 55/56, with `netif-ipv6-udp-bind` returning TIMEOUT (-6). All other IPv6 cases, including the WebRTC matrix, pass in that run. The initial raw failure is retained.
- Three subsequent uncached, serial invocations of that complete IPv6 suite pass **56/56 each**. A real OS socket comparison also passes both bound/unbound and same/two-socket loopback cases. No source fix, timeout increase, case deletion or skip was applied. The initial timeout is not reproducible in these comparisons, but its root cause is **unresolved**; the repeats do not erase it or establish a repair.
- Full macOS build: **4088 targets PASS**. Full S31 configuration graph: **4088 targets analyzed successfully**, zero actions executed by that analysis; this is distinct from native builds.
- Actual S31 native builds: **9/9 PASS** — Loader, Core, Audio, Audio Decoder, Net/TLS, PAL-WebRTC, existing iperf, WebRTC performance and board diagnostic. The migrated layout inputs compile successfully; no package is installed.
- Actual common-IDF builds: **4/4 PASS** — S3 AMOLED touch iperf server and IPv6 fixture, P4 reference smoke and C5 reference smoke. These do not establish device runtime acceptance.
- Independent migration audit verifies all **16** moved config files retain their original setting order/values; repeated log defaults are deduplicated. Canonical IPv6/SLAAC declarations are an intentional additional policy alignment. Range-diff review confirms the upstream-equivalent lock refresh is the only dropped commit; other replay differences are the reviewed conflict/context adaptations. Fresh review finds no further actionable code/layout defects in this change.

Private evidence is in `e2e-evidence/20261019/rebase-main/`: complete build/test logs, the first IPv6 failure, all three passing raw reports, migration audit, range-diff and continuation state. No physical hardware tests are run in this assessment. GizClaw business services are excluded; ordinary repository unit/build checks remain included.
