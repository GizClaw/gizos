# PAL API

Modem emergency-number discovery is exposed by `h2_pal_modem_get_emergency_numbers()`. The result preserves the queried table's SIM applicability and source; it does not guarantee network connectivity. See [provider behavior and integration notes](/zh/developing/drivers#emergency-numbers).

`H2_PAL_MODEM_CAPABILITY_OTA` describes modem firmware updates initiated by URL. Use `h2_pal_modem_ota_start()` and `h2_pal_modem_ota_get_status()` for initiation and progress/version verification; `get_identity().revision` reports the current firmware identifier. See [OTA integration](/zh/developing/drivers#module-ota).

<!--@include: ../.generated/api/pal.md-->
