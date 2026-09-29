# PAL Wi-Fi / Netif qualification

This independent App preserves the legacy mixed PAL App. Its 38 non-fail-fast
cases cover all 21 operations: STA 7, AP 5, Settings 4 and Netif 5. Device runner
adds `settings-restart-persistence`, qualified only on a later independent boot
of the same image version. A seed boot returns WOULD_BLOCK and is not confirmed.
Only complete successful runs with Settings/network restoration and removed
credential backup may confirm the managed App.

The fixture is an AMOLED ESP32-S3 running a temporary WPA2 AP and a real STA
client. It joins each DUT AP, obtains DHCP, stays for ten seconds, then leaves.
The DUT exercises WPA2, open and hidden AP modes; each requires a real leased
client and observed join/leave. It also scans the fixture, tests callback early
stop, tests borrowing and zero-timeout connect, rejects a wrong password while
retaining saved credentials, distinguishes `connect` from `connect_and_save`,
checks IP/DNS/MAC/route/event consistency and repeats lifecycles.

The public fixture key is deliberately synthetic. Production credentials are
read from the board's Settings, never logged, never committed. Before mutation,
launcher writes a private crash-recovery backup in `h2wifictl`; successful
restoration removes it. A private canonical HKDF digest and image version
verify that the restored original configuration survives a separate boot.
Persistence refers to normal reboot, not power-loss atomicity. There is no PAL
power-save getter; the terminal test policy is explicitly NONE, without claiming
the previous radio policy was measured. BK's SDK maps MAX_MODEM to its one DTIM
policy; this is not evidence of a distinct listen interval.

## Platform scope

| Platform | Actual test scope | Physical WLAN qualification |
| --- | --- | --- |
| DevKit | Real ESP Wi-Fi, Settings and Netif; 39 cases | Pending real board test |
| BK7258 | Real BK Wi-Fi, Settings and Netif; 39 cases | Pending real board test |
| macOS | Native Darwin Netif read-only; explicit unsupported physical Wi-Fi assembly | Unavailable |
| WASM | Worker, HOST Netif, actual browser offline/online Runtime events; unsupported Wi-Fi | Unavailable |
| iOS Simulator | Packaged SDK's production default Wi-Fi/Netif unsupported responses | Unavailable |
| Android Emulator | Packaged SDK's production default Wi-Fi/Netif unsupported responses | Unavailable |

Simulator/browser capability contract PASS never means a physical radio passed
scan, provisioning or AP tests. Desktop simulated Wi-Fi is not the native radio.
The independent `h2_pal_wifi_csi.h` capability is outside this qualification.
AP client lease events are reported separately where supplied by the actual SDK;
mandatory client inspection and fixture evidence require a real DHCP lease.

## Entrypoints

```sh
bazel test //projects/e2e/apps/pal-wifi/app:interface_coverage_test \
  //projects/e2e/apps/pal-wifi/app:restoration_test \
  //projects/e2e/targets/cc_binary/pal-wifi:desktop_contract_test \
  //projects/e2e/targets/pkg_tar/pal-wifi:browser_test
H2_IOS_SIMULATOR_UDID=<booted-test-device> make bazel-test-ios_pal_wifi_simulator_test
H2_ANDROID_SERIAL=<booted-test-emulator> make bazel-test-android_pal_wifi_simulator_test
```

Firmware packages are under `targets/h2loader_tar_zlib/pal-wifi`: `devkit`,
`bk7258_v3_202405` and `amoled-fixture`. The fixture window is ten minutes and
ends by stopping its AP, verifying saved credentials unchanged, and reconnecting
the original network. Hardware receipts must bind UID, image version, source and
package/image hashes, full case ledger, fixture MAC/IP corroboration, original
P1 and coredump baseline, empty Stage, confirmation and an independent normal
boot. Missing capabilities/fixture/cases, FAIL/BLOCKED, unfinished persistence,
cleanup errors or retained recovery backup keep the Wi-Fi gate closed.
