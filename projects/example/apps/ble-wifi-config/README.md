# BLE Wi-Fi Config Smoke App

This portable Smoke App opens a BLE provisioning window with
`libs/ble_wifi_config` and waits for a phone to use it, so the wire protocol
can be exercised against the real LiteLink Flutter app or the WeChat mini
program on a DevKit.

The app:

- publishes the provisioning GATT profile and advertises it as a connectable
  peripheral named `H2-Provision`, carrying the service UUID the applications
  filter on;
- prints the four UUIDs in their human form, so the operator can confirm the
  device from a generic BLE browser;
- logs every service event as an `H2_SMOKE_BLE_WIFI_CONFIG` line: connect,
  subscribe-driven scan start and finish, credentials received, and the
  provisioning outcome with its reason byte;
- prints the counters and the resulting station status once a phone has
  provisioned the device;
- closes the window as soon as the device is provisioned or the window
  expires, because that window is the whole authorization.

It produces no aggregate verdict: the device cannot provision itself, so a
run without a phone ends in `stage=done rc=-6` (timeout) by design.
