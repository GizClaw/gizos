# Mosaico H2Loader USB adapter

This product-owned component binds the board's generic CDC read/write/flush
functions to H2Loader's existing physical I/O contract. It does not own the
USB driver, change PAL or Runtime interfaces, or define an E2E App.

Initialize the board USB console first, then call
`h2_mosaico_loader_usb_init()` at serialized startup. H2Loader remains the sole
RX owner. Driver implementation and transport locking remain in
`boards/esp_mosaico/esp32s31:usb`.

`physical_io_test` verifies the H2Loader startup injection boundary. It belongs
to the product integration layer and is not an entry in the E2E App inventory.
