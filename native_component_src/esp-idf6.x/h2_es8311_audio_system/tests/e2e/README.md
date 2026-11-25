# ES8311 AEC diagnostics

Follow the [diagnostic API contract](h2_es8311_aec_e2e.h) for exclusive hardware access, C3 frequency and stack requirements. Run standalone algorithm/mode comparisons before starting unrelated services, then measure the product budget with its required management and audio services active. Standalone measurements do not qualify concurrent operation or acoustic cancellation.

The live probe's worker CPU percentage divides FreeRTOS runtime-counter deltas by elapsed microseconds. Enable the required FreeRTOS trace/runtime statistics with the ESP timer's microsecond counter before interpreting that percentage. Without runtime statistics, the probe reports CPU as not measured; deterministic host fixtures do not measure device timing, heap use or acoustics.
