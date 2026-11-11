# ESP-Mosaico Web board

The browser counterpart of ESP-Mosaico owns its 480 × 480 touch display, physical AI/BOOT button names and enclosure skin. Apps select `//boards/espressif/mosaico/web` and map the named buttons in their final Web entry. The common Web shell owns DOM input edges, keyboard bindings, canvas placement and Start/Stop lifecycle.

`device.html` draws the white front case, black glass, orange AI button, speaker openings, BOOT/POWER controls and USB-C port in SVG/CSS. AI and BOOT supply normal button edges through `data-h2-button`; the physical power circuit is represented by the page's Start/Stop lifecycle. The canvas keeps its native 480 × 480 backing resolution while the enclosure scales to the viewport.

## Reference material

- [Espressif V1.2 hardware guide and photographs](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html).
- [Official Mosaico button-location guide](https://mosaico.ai/en/guide).
- [ESPRESSIF Core Unit hardware page](https://mosaico.ai/en/hardware/bzWKymkXPu95), including assembly/expansion guides and `ESP-MOSAICO.3mf`.
- [Official 3MF download](https://mosaico.ai/api/v1/files/attachments/1/9734120a8783.3mf), SHA-256 `aa84074593344c37684cc2f36c58b5698ed5b695246e701ff05e49d325c176b3`.

The 3MF is a mesh-based printing project. Its main case mesh spans approximately 44.898 × 44.898 × 11.1 mm and its metadata identifies the MakerWorld Exclusive License. This package contains an independent SVG/CSS illustration referenced to the official photographs; the upstream model remains at its official download location. The skin is a visual simulator, not a manufacturing drawing.

## Validation

The G99 consumer exercises real Main navigation, the clickable AI return button, BOOT screen-off/wake and cleanup:

```sh
bazel test --config=macos_arm64 \
  //projects/g99/targets/pkg_tar/web/mosaico:device_controls_test \
  //projects/g99/targets/pkg_tar/web/mosaico:navigation_test
```
