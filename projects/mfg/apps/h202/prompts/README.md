# H202 MFG voice prompts

Each `zh/<name>.pcm` is raw 8 kHz, mono, signed 16-bit little-endian PCM
with no header. The committed bytes are authoritative. The MFG build turns
each declared file into a C array; the player upsamples it to the board's
16 kHz track. Regenerate only when wording or parameters change.

| File | Wording | Spoken text |
| --- | --- | --- |
| `charge_insert.pcm` | 请插入充电线。 | Same |
| `charge_remove.pcm` | 请拔出充电线。 | Same |
| `charge_passed.pcm` | 充电检测成功。 | Same |
| `nfc.pcm` | 请将测试卡靠近感应区。 | Same |
| `nfc_passed.pcm` | NFC检测成功。 | Same |
| `wifi_passed.pcm` | Wi-Fi测试成功。 | Same |
| `cellular_passed.pcm` | 4G连接成功。 | 四G连接成功。 |
| `mic_left.pcm` | 左侧麦克风检测，请对着左侧麦克风说话。 | Same |
| `mic_right.pcm` | 右侧麦克风检测，请对着右侧麦克风说话。 | Same |
| `listen.pcm` | 请按肚子上的按键确认两侧麦克风正常。 | Same |
| `power.pcm` | 请按下方开关键。 | Same |
| `plus.pcm` | 请按音量加键。 | Same |
| `minus.pcm` | 请按音量减键。 | Same |
| `aging.pcm` | 测试完成，开始老化。 | Same |
| `waiting.pcm` | 请等待刷入固件。 | Same |
| `charge_failed.pcm` | 充电检测失败。单击肚子上的按键重试。 | Same |
| `nfc_failed.pcm` | NFC检测失败。单击肚子上的按键重试。 | Same |
| `wifi_failed.pcm` | Wi-Fi测试失败。单击肚子上的按键重试。 | Same |
| `cellular_failed.pcm` | 4G连接失败。单击肚子上的按键重试。 | 四G连接失败。单击肚子上的按键重试。 |
| `mic_failed.pcm` | 麦克风检测失败。单击肚子上的按键重试。 | Same |
| `aging_failed.pcm` | 老化失败。单击肚子上的按键重试。 | Same |
| `incomplete.pcm` | 检测未全部通过，请关机后重新开机复测。 | Same |
| `unsaved.pcm` | 关机失败，进度未保存。单击肚子上的按键重试关机。 | Same |
| `poweroff_failed.pcm` | 进度已保存，关机执行失败。单击肚子上的按键重试关机。 | Same |

Generated on macOS with the built-in Tingting voice at rate 210. `4G` is
spoken as `四G` so the voice reads it naturally. For one file:

```sh
say -v Tingting -r 210 -o /tmp/<name>.aiff '<spoken text>'
afconvert -f WAVE -d LEI16@8000 -c 1 /tmp/<name>.aiff /tmp/<name>.wav
python3 -I -c 'import sys,wave; w=wave.open(sys.argv[1]); open(sys.argv[2],"wb").write(w.readframes(w.getnframes()))' /tmp/<name>.wav zh/<name>.pcm
```

A different macOS release may produce different bytes. Listen to every
changed prompt on hardware before committing it, and declare any new file
explicitly in `../BUILD.bazel`.
