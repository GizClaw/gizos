# Modem Smoke

`modem-smoke` is a shared portable Smoke App. It records each modem
stage independently: modem identity and IMEI, SIM state, registration, PPP,
and a bounded ICMP echo to `1.1.1.1`. A missing SIM is recorded and the
dependent network stages are left unattempted; the App does not calculate an
aggregate pass or fail verdict.

The current Waveshare H2Loader launcher owns modem wiring, image confirmation,
and board acceptance; none of those product concerns are part of this App.
