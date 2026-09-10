# AMOLED storage direction

The selected resource representation is vector/path commands plus Lua note data;
see [VECTOR-DESKTOP.md](VECTOR-DESKTOP.md). Earlier raster-pack, SD and repartition
proposals are superseded. There is no pending repartition approval or selected
alternative Flash layout.

Historical board inspection reported 16 MiB Flash, 8 MiB PSRAM, an 8 MiB app
partition and PSRAM XIP. The user confirmed an SD card is available. Card mounting,
bandwidth and update integration have not been validated for this application.

Measure the completed ESP32 vector firmware and peak memory before choosing
external storage. The desktop vector resource size is not a complete firmware
size and does not certify that the existing partition is sufficient. Preserve the
current partition table and card contents during the pending migration. Historical
raster measurements are summarized in [AMOLED-MIGRATION.md](AMOLED-MIGRATION.md).
