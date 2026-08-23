# BLE Broadcaster Smoke App

This shared portable Smoke App exercises the common BLE advertising PAL
contract and records each stage. It runs the same sequence on every consumer:

- legacy non-connectable advertising with an explicit stop;
- Extended Advertising on LE Coded primary PHY and LE 2M secondary PHY with
  an explicit stop;
- automatic termination by duration;
- automatic termination by maximum advertising-event count;
- an SID of 7 and an advertising payload larger than the 31-byte legacy limit.

After the lifecycle stages complete, the app leaves the Extended Advertising
set active so the paired BLE Observer app can record PHY selection, SID,
payload, and data completeness on either supported board. It does not produce
an aggregate test verdict.
