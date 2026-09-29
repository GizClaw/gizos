# PAL Wi-Fi / Netif qualification

This independent App preserves the legacy mixed PAL App. Its 38 non-fail-fast
cases cover all 21 operations: STA 7, AP 5, Settings 4 and Netif 5. Device runner
adds `settings-restart-persistence`, qualified only on a later independent boot
that reads the prior fixed v1 canonical Settings record. A seed boot returns
WOULD_BLOCK and is not confirmed. ESP rolls back unconfirmed same-slot reboots;
the verified seed may therefore precede a different managed App image. Reports
retain the actual prior seed version. The final qualified image then passes a
separate ordinary App reboot after confirmation.
Only complete successful runs with Settings/network restoration and removed
credential backup may confirm the managed App.

The fixture is an AMOLED ESP32-S3 running a temporary WPA2 AP and a real STA
client. It joins each DUT AP, obtains DHCP, stays for ten seconds, then leaves.
It is a fixed R15 test tool: its archived package/image and three historical
source inputs are pinned to the actual Git blobs used to build it. The live
raw-byte audit verifies those blobs; portable receipt checks validate the
captured provenance without requiring old commits in a shallow CI checkout.
The DevKit and BK7258 qualification images remain bound to current source.
The DUT exercises WPA2, open and hidden AP modes; each requires a real leased
client and observed join/leave. It also scans the fixture, tests callback early
stop, tests borrowing and zero-timeout connect, rejects a wrong password while
retaining saved credentials, distinguishes `connect` from `connect_and_save`,
checks IP/DNS/MAC/route/event consistency and repeats lifecycles. AP+STA
route selection uses the independently saved infrastructure AP when available,
while the fixture is the real DUT AP client; it still requires both interfaces
usable and actual AP→STA route switches. No saved infrastructure credentials
means it uses the controlled fixture AP.

The public fixture key is deliberately synthetic. Production credentials are
read from the board's Settings, never logged, never committed. Before mutation,
launcher writes a private crash-recovery backup in `h2wifictl`; successful
restoration removes it. A private canonical HKDF digest and image version
verify that the restored original configuration survives a separate boot or
managed upgrade. Image identity remains exact per receipt, without relabelling
the prior seed as the final artifact.
Persistence refers to normal reboot, not power-loss atomicity. There is no PAL
power-save getter; the terminal test policy is explicitly NONE, without claiming
the previous radio policy was measured. BK selects MAX_MODEM using the firmware dynamic listen interval (ten beacons)
and checks the SDK readback. MIN_MODEM uses its minimum recommended listen interval (one
beacon, including the next DTIM). The App cycles MAX→MIN→MAX with SDK readback;
reconnect reapplies the selected policy. These checks do not
measure radio power consumption.

The event auditor validates all 11 Wi-Fi Runtime event kinds plus default
Netif changes. It requires GOT_IP before LOST_IP, a client JOIN before a
real lease GRANTED, and a same-MAC/same-IP RELEASED after that grant. A client
may leave before the release event; leaving without an accepted lease must
not synthesize one. Each WPA2/open/hidden AP case waits for its own accepted
lease, and the fixture receipt must independently show a fresh real client.
After the first WPA2 client leaves, the App waits for a second accepted lease
and release while consuming only Runtime events, without calling AP status or
client APIs; this checks that event-only consumers receive them autonomously.

BK's pinned AP SDK public `connect`/`disconnect` only change local state.
The provider uses paired AP/CP RPCs for actual radio association/disassociation,
checks the CP response payload, and preserves the STA service for reconnect.
Fresh SDK `STA_START` already initiates authentication; it is not started twice.
Repo-owned CMake builds guarded corrected SDK copies inside the build tree and
keeps the sealed SDK pristine. The local STA address-sync correction also avoids
sending a STA lease to the CP AP configuration. Regression probes compile the
actual generated SDK corrections and reject old/failed CP responses.

## Platform scope

| Platform | Actual test scope | Physical WLAN qualification |
| --- | --- | --- |
| DevKit | Real ESP Wi-Fi, Settings and Netif; R23 old source managed and normal boot each 39/39, fresh actual peer leases and complete restoration | New event source needs physical requalification |
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
  //projects/e2e/apps/pal-wifi/app:event_contract_test \
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
