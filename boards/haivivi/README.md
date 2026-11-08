# Haivivi boards

These BSPs were promoted from `h2vivi/firmwares`. Each board retains its existing public C interfaces, wiring, peripheral IDs, layouts and firmware identities. The directory names identify the product and physical model; renaming a directory does not rename devices or update packages.

| BSP directory | Existing firmware identity |
| --- | --- |
| `h106_zero_esp_v3_0/esp32s3` | `zero_esp_v3_0` |
| `h106_tiga_esp_v4_2/esp32s3` | `tiga_esp_v4_2` |
| `h106_zero_bk_2_0/bk7258` | `zero_bk_2_0` |
| `h200_esp32s3/esp32s3` | `h200` or `h200_v2`, selected by the existing battery profile |

The H200 identities select the original and Donghui battery profiles of the same physical BSP; they do not establish separate PCB revisions. Lucky Kitty v1.3 is retired and is not maintained here.

H106 product Main, Loader, MFG, their shared factory code and Loader UI remain owned by Firmwares. Public manufacturing acceptance belongs to [the MFG project](../../projects/testing/mfg/README.md), with final image composition owned by its entries. This directory owns hardware configuration and Runtime/PAL composition only; it does not import private factory workflows, contain product credentials or perform provisioning. The BK model is Zero BK 2.0 (LED GPIO4, modem enable GPIO6); the previously mislabeled 1.0 entry is retired. There is no H200 MFG program.

Run the existing BSP host regressions with the appropriate host configuration, for example:

```bash
bazel test --config=macos_arm64 //boards/haivivi/...
```

Host regressions and graph analysis do not establish device acceptance. Firmware builds require a final entry, its declared SDK configuration and its selected board layout.
