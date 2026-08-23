# Wi-Fi CSI Smoke App

This is a shared portable Smoke App for displaying raw Wi-Fi CSI reception:
link state, provider errors, basic radio metadata, frame count, and a bounded
I/Q sample curve. It uses a saved STA configuration and never accepts Wi-Fi
credentials from the screen or command line.

H2Loader board launchers remain consumers and own saved-network provisioning,
image confirmation, and board-specific provider wiring.

On an otherwise quiet link, the App sends one ICMP echo to the saved network's gateway each second when the optional Runtime Net PAL is available. The reply supplies low-rate traffic for observable CSI frames; the router does not need custom software, and the ICMP result is not treated as a sensing result.

It intentionally does not infer people, sleep, posture, gesture, occupancy,
location, or any other semantic result. Those require a separately designed
and validated algorithm.

BK7258 uses the SDK host-capture path and renders valid packed I/Q samples
delivered by its public callback. ESP-IDF providers also retain complex I/Q
samples.
