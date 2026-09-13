# Loader candidate pre-confirm reset recovery

Device UID: `3ce9e275d7aa`; UART1, 460800 baud. No manual reset or USB DL between candidate installation and recovery. The test-only `loader_trial_crash_package` uses the production Loader graph, layout and task policy, plus a boot-stage 105 hook that records a Loader-origin assertion marker `0x48324c43` and calls `system_reset()` before confirmation. This is deterministic pre-confirm failure injection, not a test of every hardware exception or of power loss.

Candidate package SHA-256: `6ba6256dc9f5ed1880a4e1c1914ceffb7be8bdfce18ace0508e5b08d1cd199ff`. After automatic recovery, P1 still runs image `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`; the failed Loader remains in P2 and Stage, with `last_result=-7`. The 2096-byte coredump has Loader-origin bit `0x80000000`, stage 105 and the expected caller marker. Commands remained available to export the record, abort Stage and request another Loader reboot. The reboot reached P1 READY; its host command kept waiting and was subsequently interrupted, so that host command is not recorded as a successful end-to-end reconnect.

## Captured device evidence

```text
[00:00:12.205][Info]: [UPDATE]reverse_item_addr:37c0a0 name:VM cfg_adr:6f5000 len:8000 index:0 res:80

H2_JIELI_UPDATE_BURN_CALLBACK error=0
H2_JIELI_UPDATE_BURN call=0 pend=0 result=0
H2_JIELI_UPDATE_EXIT_ENTER
H2_JIELI_LOADER_COMMIT mode=warm boot_info=unpublished
H2_JIELI_POWER_REBOOT running=1 next=2 committed=1 reason=0
H2_JIELI_REBOOT_SCHEDULED timer=8412 delay_ms=2000
H2_JIELI_LINK ms=12390 rx_phase=0 rx_calls=0 rx_busy=0 rx_bytes=0 frames=0 last_flags=0 last_frame_ms=0 fs=fopen-return
[00:00:12.357][Info]: [UPDATE]reverse_item_addr:37c0c0 name:PRCT cfg_adr:0 len:6f5000 index:0 res:82

[00:00:12.357][Info]: [UPDATE]reverse_item_addr:37c0e0 name:BTIF cfg_adr:6fd000 len:1000 index:1 res:81

[00:00:12.357][Info]: [UPDATE]find reserve file end

[00:00:12.360][Info]: [UPDATE]kill update task : dw_update

[00:00:12.372][Info]: [OS]task_create: h2loader/appcmd, 0x2017fae
[00:00:12.372]H2_LOADER_READY board=jieli_ac791n_devkit target=wl82 transport=iostreamikcp-usb0
[00:00:12.372]H2_JIELI_LOADER_EXIT code=0
H2_JIELI_LINK ms=13400 rx_phase=0 rx_calls=0 rx_busy=0 rx_bytes=0 frames=0 last_flags=0 last_frame_ms=0 fs=fopen-return
H2_JIELI_REBOOT_CALLBACK task=sys_timer
H2_JIELI_REBOOT_EXECUTE reset=core
H2_JIELI_LOADER_BOOT reset_reason=0x10 next=runtime_config
H2_JIELI_CRASH_RECOVERY state=active transport=board-console app_boot=blocked
H2_JIELI_LOADER_OK step=runtime_config
H2_JIELI_LOADER_ENTER step=coredump_flush
H2_JIELI_LOADER_OK step=coredump_flush stored=1
H2_JIELI_LOADER_ENTER step=loader_platform
H2_JIELI_BOOT_INFO result=0 base=0x4020 bytes=928253 version=6 logical=1
H2_JIELI_WARM_BOOT count=1378375596 last_base=0x37c020
H2_JIELI_WARM_SNAPSHOT marker=504d3248/47424434 stage=105 result=0 log_magic=81320001 head=0 total=0
H2_JIELI_TRIAL_ROLLBACK app_bootable=0 action=command-mode
H2_JIELI_LOADER_OK step=loader_platform
H2_JIELI_LOADER_ENTER step=runtime_init
H2_JIELI_LOADER_OK step=runtime_init
H2_JIELI_LOADER_ENTER step=firmware_info
H2_JIELI_LOADER_OK step=firmware_info
H2_JIELI_ACTIVE_IDENTITY source=pref
H2_JIELI_LOADER_ENTER step=app_run
H2_LOADER_READY board=jieli_ac791n_devkit target=wl82 transport=iostreamikcp-usb0
[00:00:00.100]========= system reset reason: SOFT =========

[00:00:00.100]~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
[00:00:00.100]   WL82(AC791N) CHIP_ID: 0x6f01  setup_arch Sep 12 2026 18:10:38
H2_LOADER_STATUS board=jieli_ac791n_devkit target=wl82 chip=ac791n device_uid=3ce9e275d7aa capabilities=0x00000005 command_availability=0x00081f1e active_role=loader active_version=bazel-native-artifacts active_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8 active_image_size=928317 running_partition=1 next_partition=1 boot_intent=auto stage_valid=1 stage_package_checksum=6ba6256dc9f5ed1880a4e1c1914ceffb7be8bdfce18ace0508e5b08d1cd199ff stage_package_size=916728 stage_image_checksum=5cb8d6763822b1e50af3296bc690d0ad2e624b9512cd05ab209f4dcf40e64772 stage_image_size=928317 stage_role=loader stage_version=bazel-native-artifacts stage_board=jieli_ac791n_devkit stage_target=wl82 partition_1_valid=1 partition_1_package_checksum=3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2 partition_1_package_size=916624 partition_1_image_checksum=3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8 partition_1_image_size=928317 partition_1_role=loader partition_1_version=bazel-native-artifacts partition_1_board=jieli_ac791n_devkit partition_1_target=wl82 partition_2_valid=1 partition_2_package_checksum=6ba6256dc9f5ed1880a4e1c1914ceffb7be8bdfce18ace0508e5b08d1cd199ff partition_2_package_size=916728 partition_2_image_checksum=5cb8d6763822b1e50af3296bc690d0ad2e624b9512cd05ab209f4dcf40e64772 partition_2_image_size=928317 partition_2_role=loader partition_2_version=bazel-native-artifacts partition_2_board=jieli_ac791n_devkit partition_2_target=wl82 last_result=-7 mfg_mode=1 mfg_steps=0000000000000000000000
00000000: 3008 0000 4832 4352 0200 0000 1a7d f483  0...H2CR.....}..
00000010: 0000 0080 6900 0000 434c 3248 0000 0000  ....i...CL2H....
00000020: 0008 0000 44fa 0000                      ....D...
```
