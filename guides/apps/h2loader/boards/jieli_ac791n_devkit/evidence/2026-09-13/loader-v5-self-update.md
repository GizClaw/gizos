# Distinct-image Loader update v4 to v5

UART1, 460800 baud, UID `3ce9e275d7aa`. No USB DL or manual reset.
The staged package was fully acknowledged and verified:
`cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8`
(916907 bytes). This was not a same-image install: old P1 image SHA was
`20a1aa73e6213bc76fbc35bdf8d4582f8015be529386cf2830b0b7f0557ac11f`.

The upgrade log captured, in order:

```text
H2_JIELI_LOADER_TRIAL confirmed=1 publish_gate=before-copy-p1
H2_JIELI_LOADER_HEADER published=1 confirmed=1
H2_JIELI_STARTUP_EVENT event=2 code=0
H2_JIELI_BOOT_INFO result=0 base=0x4020 bytes=928509 version=14 logical=1
H2_JIELI_STARTUP_EVENT event=4 code=0
```

After the monitor was stopped, an independent successful status query reported
P1 running Loader image
`fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`,
matching package metadata in P1/P2, `stage_valid=0`, `last_result=0`.
This validates candidate confirmation, deferred native-header publication,
copy-back and final convergence for this update.

The log contains one `TRIAL_ROLLBACK` on the initial **old v4** startup,
before installation event1. v4 mistakenly recreated a P2 attempt when copying
the confirmed Loader to P1. v5 records an attempt only for the P2 destination;
the final v5 P1 startup after native version14 has no rollback marker.
Do not remove the old marker from the source log or present the entire run as
having no rollback messages. Source logs are `deferred-v5-install.log` and
`deferred-v5-after.status`. This test does not cover interruption during writes.
