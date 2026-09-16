# Installed App relaunch with v5 Loader

UID `3ce9e275d7aa`, UART1 460800. After restoring the color-bar App,
only `reboot loader` and `reboot app` were issued between the three independent
status snapshots. No send, install or Stage modification occurred in between.
Each monitor was confirmed stopped before the next status request.

| Snapshot | Running partition | Active image SHA | Stage valid | Last result |
| --- | --- | --- | --- | --- |
| Before | 2 | `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` | 0 | 0 |
| Loader | 1 | `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d` | 0 | 0 |
| After | 2 | `072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23` | 0 | 0 |

The relaunch log includes `H2_JIELI_POWER_REBOOT running=1 next=2 committed=0`,
`JIELI_APP_CONFIRM result=OK code=0 display=0 transport=0`, and heartbeats at
11960, 16960, 21960 and 26960ms. Sources are
`deferred-v5-relaunch-{before,loader,after}.status` and
`deferred-v5-relaunch-app.log`. This verifies software-reboot handoff, not
physical power removal or interrupted writes.
