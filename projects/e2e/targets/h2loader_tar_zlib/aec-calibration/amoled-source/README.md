# Independent AEC probe source

This test-only AMOLED App plays the exact mono S16LE probe supplied by the
Mosaico calibration runner through a separate physical speaker. It never
injects PCM into the DUT capture path. Its HTTP control service uses port 18081;
`POST /probe` accepts exactly 512 samples at 16 kHz, and `POST /stop` stops the
speaker. Both require the explicit `X-AEC-Token` fixture credential. A successful
response confirms the Audio PAL transition. Playback expires after 60 seconds;
a stuck control exchange or failed cleanup resets the helper instead of
acknowledging an unknown audio state.

The source uses 70% speaker volume and restores its previous control value on
stop. Record physical placement and the admitted image/UID alongside the DUT
report. This percentage is not an absolute sound-pressure measurement.

Build with the existing `esp32s3` configuration and explicit CMake inputs
`H2_E2E_WIFI_SSID`, `H2_E2E_WIFI_PASSWORD`, and `H2_AEC_SOURCE_TOKEN`. Supply them
through a private operator bazelrc; do not commit credentials. The Mosaico
launcher uses `--define=mosaico_e2e=aec-calibration`, the same token and an
`H2_AEC_SOURCE_URL` pointing to this helper. Only that calibration image opts
into the board's AEC path. Other Mosaico workloads retain their existing AEC
setting.

The first calibration tests speaker/microphone percentages (25,20) and (50,40),
with the App's unchanged default levels and limits. It does not search the full
volume/gain range. The reusable App and independent public report validator
remain under `projects/e2e/apps/aec-calibration`. No PAL package is introduced.

The fixture disables Wi-Fi modem sleep and enables TCP_NODELAY for small control
responses; the calibration control deadline remains unchanged. Response logs
record control duration and send status, without PCM or credentials.

AMOLED retains its existing legacy volume mapping: 25% maps to DAC register
50 (-70.5 dB), while 70% maps to 142 (-24.5 dB). The fixture uses 70% so the
near-end probe is not suppressed by an almost silent control setting. This
does not change the board driver, DUT candidates or acoustic qualification
limits, and is not a claim of matched sound pressure between the boards.
