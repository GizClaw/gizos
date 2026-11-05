# Zero BK 1.0

`zero_bk_1_0` is the H106 Zero BK7258 physical board. The AP owns the H106 Runtime-facing peripherals and UART1 (GPIO 0/1, 460800) for logs and H2Loader managed iKCP; the CP owns the BK controller and the UART0 download port.

The confirmed hardware contract is defined by:

- `include/h2_zero_bk_1_0_board_config.h` for stable board identity, GPIO assignments, buses, display geometry, and physical `periph_id` values.
- `ap.defaults` and `cp.defaults` for board-wide Armino capabilities.
- `ap/usr_gpio_cfg.h` and `cp/usr_gpio_cfg.h` for side-specific boot pin mux.
- `layouts/loader` and `layouts/h2loader` for the fixed Loader (2448 KiB) and XIP App (5032 KiB) partition tables, which mirror each other through `s_app`, and RAM regions.

MFG uses the dedicated `layouts/mfg` AP/CP defaults to disable Bluetooth host/controller and keeps UART iKCP management. It aliases this board's Loader partition table and existing GPIO/RAM inputs; ordinary Loader and Main retain BLE.

The AP pin map follows the tested `H106-BK-ZERO` project. Its corrected production wiring uses GPIO 4 for the RGB LED and GPIO 6 for modem enable. The ST7789 display uses SPI0 on GPIO 14–16; GPIO 17 is display DC rather than SPI MISO.

The external QSPI0 flash is board storage. An image launcher must mount and map its `/dl` and `/data` roots explicitly; it must not inherit the SD-card mapping from `bk7258_v3_202405`.
