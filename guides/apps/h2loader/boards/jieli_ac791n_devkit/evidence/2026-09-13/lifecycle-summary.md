# Historical Loader lifecycle — 2026-09-13

These are historical checkpoints, not acceptance of the current source. Run identities, outcomes and status transitions are retained below; no raw capture is required to read them. Missing fields were not recorded and must not be inferred from another run.

## Retained acceptance facts: ble-diagnostic-full

Historical run summary transcribed from `ble-diagnostic-full.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:8534d718f646` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `914869` |
| loader_firmware.sha256 | `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `324394` |
| identity.active_checksum | `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a`; `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| identity.image_checksum | `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45`; `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_2_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |

## Retained acceptance facts: ble-full-uart-observed

Historical run summary transcribed from `ble-full-uart-observed.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:8534d718f646` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `914869` |
| loader_firmware.sha256 | `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `339714` |
| identity.active_checksum | `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a`; `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| identity.image_checksum | `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45`; `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_2_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |

## Retained acceptance facts: deferred-clean-uart-e2e

Historical run summary transcribed from `deferred-clean-uart-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `25` |
| case_results | `PASS=25` |
| result | `PASS` |
| rc | `0` |
| uart_endpoint | `/dev/cu.usbserial-20131240` |
| uart_baud_rate | `460800` |
| repeat | `1` |
| monitor_duration_ms | `1000` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `916624` |
| loader_firmware.sha256 | `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `284650` |
| identity.active_checksum | `3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8` |
| identity.package_checksum | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a`; `4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9`; `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2` |
| identity.image_checksum | `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45`; `3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[stage-abort-after-send], record.case[monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=3dba7933c7dc3c7dcf381aae50282713d3ae184d652d6f8b2c66b06b27cc0475; partition_1_package_checksum=4723f3445062aea7ec76d2000f1bdd933fffb6b46c9ab5e3cdd12db94f651bf3; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_2_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |

## Retained acceptance facts: deferred-v2-ble-e2e

Historical run summary transcribed from `deferred-v2-ble-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:8534d718f646` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `916624` |
| loader_firmware.sha256 | `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `333637` |
| identity.active_checksum | `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9`; `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| identity.image_checksum | `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_2_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_2_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8; partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |

## Retained acceptance facts: deferred-v5-ble-e2e

Historical run summary transcribed from `deferred-v5-ble-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:8534d718f646` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `916907` |
| loader_firmware.sha256 | `cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `342092` |
| identity.active_checksum | `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a`; `cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| identity.image_checksum | `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45`; `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_2_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |

## Retained acceptance facts: deferred-v5-uart-e2e

Historical run summary transcribed from `deferred-v5-uart-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `25` |
| case_results | `PASS=25` |
| result | `PASS` |
| rc | `0` |
| uart_endpoint | `/dev/cu.usbserial-20131240` |
| uart_baud_rate | `460800` |
| repeat | `1` |
| monitor_duration_ms | `1000` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `916907` |
| loader_firmware.sha256 | `cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `268602` |
| identity.active_checksum | `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9`; `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| identity.image_checksum | `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_2_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_2_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_2_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d; partition_1_package_checksum=cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |

## Retained acceptance facts: restored-ble-e2e

Historical run summary transcribed from `restored-ble-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `FAIL=3, PASS=19` |
| result | `FAIL` |
| rc | `-7` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:8534d718f646` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `914869` |
| loader_firmware.sha256 | `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `0` |
| summary.cases | `22` |
| summary.passed | `19` |
| summary.failed | `3` |
| summary.elapsed_ms | `319108` |
| identity.active_checksum | `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |
| identity.package_checksum | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a`; `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7`; `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| identity.image_checksum | `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45`; `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_2_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| record.case[install-crash-app] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_2_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |

## Retained acceptance facts: restored-uart-e2e

Historical run summary transcribed from `restored-uart-e2e.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `25` |
| case_results | `PASS=25` |
| result | `PASS` |
| rc | `0` |
| uart_endpoint | `/dev/cu.usbserial-20131240` |
| uart_baud_rate | `460800` |
| repeat | `1` |
| monitor_duration_ms | `1000` |
| app_firmware.bytes | `855805` |
| app_firmware.sha256 | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| loader_firmware.bytes | `914869` |
| loader_firmware.sha256 | `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| crash_firmware.bytes | `298639` |
| crash_firmware.sha256 | `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `283797` |
| identity.active_checksum | `fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9` |
| identity.image_checksum | `fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36`; `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`; `5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9`; `0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45` |
| identity.package_checksum | `37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9`; `0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7`; `54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=1; partition_1_image_checksum=fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9; stage_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; stage_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=fc786a69b1c577f0dfa64d0e95cf1481a926724fed9f33dce2ae4500336e0e36; partition_2_image_checksum=072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23; partition_2_package_checksum=37c3b68f890d3fbec91fdb73c8fdec4285c4aa673dee666826d23c145a3f9ab9` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_2_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=5cc3c85665cc20eb64dab9882fe18e22ed16810be8fff2797f25f95775b51df9; partition_1_package_checksum=0d0596e2e9c430f614c3af9caff615271c21e438105fb56a08467e97837aa8f7; partition_2_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; partition_2_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a; stage_image_checksum=0fa569a6661b6f270550dd3380206ad24b078a7b5d7b5861324455995fb53a45; stage_package_checksum=54f0046c7f603a1d071233c0db817534b104547b4ca7e8457af6014c69d2746a` |
