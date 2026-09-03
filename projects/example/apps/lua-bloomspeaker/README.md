# lua-bloomspeaker

`lua-bloomspeaker` is the portable audio-reactive UI for the first stage of a Bluetooth intercom. The same Lua scene runs on Desktop and the 368x448 AMOLED board. It captures microphone frames through Runtime Audio and maps RMS, peak, and high-frequency energy to particle blooms on a pure black background. On AMOLED, holding PWR for two seconds performs a software shutdown, while holding the BOOT/function key for one second starts pairing or disconnects an active conversation. Pairing remains active only while that key is held; releasing it before a connection is established returns to idle.

The particle lifecycle follows the `PARTICLES` mode from SonicVis: quiet input continuously emits a small ambient stream, audible peaks launch faster and larger bursts, velocity decays with friction, and each particle shrinks and fades with its remaining life. The emitter stays centered while idle. A stationary one-second mouse press enters or leaves pairing on Desktop; AMOLED touch no longer controls pairing. Pointer movement never drags or emits particles. Pairing uses the yellow theme: a restrained center seed is replaced once per second by a full-strength radial bloom, producing a regular heartbeat that reaches maximum spread before fading ahead of the next beat. A connected session places equal-sized blooms near the lower and upper edges: local audio uses the lower bloom and received audio uses the upper green bloom. Each side is capped at 10 active particles within its reserved 64/32 pool partition, so repeated input cannot grow render work. A particle first expands radially from its source; as soon as it reaches maximum size it follows a quadratic curved path into the opposite bloom while shrinking and fading. The RMS noise floor is 0.00018 and the peak floor is 0.00035; both use a square-root soft knee. A second visual-only knee keeps the amplified quiet baseline out of radial bursts: RMS supplies most of the bloom size while peak supplies attack timing, and the resting ambient motion stays compact below 0.28 mapped RMS. This preserves microphone sensitivity while making normal speaking volume visibly larger than the idle bloom.

Trails are retained framebuffer history, not line primitives. Every frame fades the existing RGB565 framebuffer toward black by the time-correct equivalent of a 15% black overlay at 60 Hz, then draws the current particle heads. A native spatial-dither path preserves the same decay duration when the Desktop renderer runs faster than RGB565 can represent with a single channel step. This keeps the trail long and softly graduated without allocating Lua trail-point arrays. During a call, particles and their retained trails stay in a centered 128-pixel corridor. Only that corridor is faded and transferred to the AMOLED after the first call frame, avoiding a full 368x448 RGB565 upload on every frame. Idle uses the same partial-update strategy with a 160x160 center square. The pairing dirty rectangle is derived from the heartbeat's maximum speed, lifetime, two drag phases, spawn spread, particle radius, and padding. Particles are no longer killed at an arbitrary square edge, avoiding visible clipping while retaining a bounded partial transfer.

The animation loop is uncapped. Particle generation is time-based rather than frame-count-based, so idle density and burst intensity stay consistent across Desktop and AMOLED frame rates. The AMOLED power key mapping is retained; Desktop maps Escape to the same back/power component.

Desktop target:

```sh
bazel run --config=macos \
  //projects/example/targets/cc_binary/lua-bloomspeaker:example-lua-bloomspeaker
```

AMOLED package:

```sh
source ../firmware-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-bloomspeaker/amoled:package
```

The native phase-2 boundary is present: Lua handles the gesture and scene, while a native controller owns intercom state and lock-free level snapshots. Bluetooth discovery, connection security, transport, and codec workers live behind that boundary.

## Performance review

The Lua render loop intentionally remains one coroutine. Gizos exposes `runtime.spawn/yield`, backed by a bounded ready queue, but all coroutines in a single `lua_State` execute serially on that VM's fixed worker. Moving drawing, touch polling, or state sampling to another Lua coroutine would add scheduler work without adding CPU parallelism, and it would still serialize on the same display object.

Work that can overlap the UI is already native: BLE/iKCP and Opus capture run in separate bounded tasks, while decoded playback uses the Audio System's bounded track queue. Capture submits encoded frames non-blocking to iKCP's bounded 4 KiB transmit queue; backpressure drops one real-time frame instead of stalling microphone capture, and the sequence gap drives remote Opus PLC. The Lua/C state sample used every 33 ms returns scalar values instead of allocating a table. The remaining frame-time cost is the full-screen RGB565 history fade, anti-aliased particle heads, and display transfer; an async Lua queue cannot reduce those costs. Particle count and per-frame geometry remain strictly bounded at 96 slots.

Pairing uses a 25-byte legacy-advertising beacon containing an ephemeral 40-bit device tag, random 64-bit ordering ticket, epoch, state, and claim target. Devices observed for at least 600 ms are sorted by `(ticket, tag)` and paired as adjacent entries. With three devices, the first two mutually claim one another and the unpaired third device keeps waiting. A two-phase mutual claim prevents crossed connections; an unmatched claim expires after 2.2 s. The lower-ranked device becomes the peripheral and the higher-ranked device becomes the central, so both ends never race to initiate the same link.

The conversation stream uses LE Secure Connections, ATT MTU negotiation, LE 2M when available, and the repository iKCP transport. Audio is 16 kHz mono Opus VOIP at 18 kbit/s in 20 ms frames. Playback starts with a three-frame jitter reserve and a six-frame device queue; dropped capture frames are recovered from Opus in-band FEC when the following packet is available, with PLC used for longer gaps instead of allowing latency to grow without bound. Opus DTX reduces encoded background sound during silence. A five-second audio diagnostic reports sent/dropped/received frames, sequence gaps, FEC/PLC use, and the active speaker volume to the serial log.

The AMOLED ES8311 path runs direct ESP-SR full-duplex AEC at 16 kHz with one microphone channel and one playback-reference channel. Codec register 0x44 is set to `ADC + DACR`, so the canceller receives the actual outgoing speaker signal rather than a duplicate microphone channel. Its nonlinear residual echo suppression is set to aggressive; a separate broadband noise suppressor or AGC is not enabled. The BloomSpeaker AMOLED target opts into an 80 MHz SH8601 pixel clock; the shared board keeps its driver clock default for other projects. The target also requests 12 dB microphone PGA gain and 100% speaker volume through its audio profile. Its I2S DMA sizing, 16-frame microphone queue, pinned media core, and audio task priorities follow the proven TIGA full-duplex layout without changing the AMOLED board defaults for other applications. The ES8311 PGA conversion uses the codec's documented 3 dB steps, while playback remains at unity track gain to avoid digital clipping.

The automatic six-digit passkey is derived from ephemeral values visible in the pairing advertisements. It prevents accidental cross-pairing and enables link encryption, but it does not authenticate against an active nearby MITM. A production version must provision a non-public product secret, use an out-of-band QR/NFC secret, or add a user-confirmed numeric comparison.
