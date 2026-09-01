# Lua Cosmic Drift on AMOLED

该 H2Loader App 使用 AMOLED 368 x 448 Display、FT3168 Touch、Boot button 和 PSRAM Memory provider，运行与 Desktop 相同的 portable App 和 compiled Lua resource。Touch down/move 持续把玩家加速方向指向接触位置；Boot button 映射到 App 的 Back component。

构建：

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/amoled:package
```

设备已经可通过 H2Loader 通信时，只能按 [AMOLED H2Loader](./h2loader) 使用 H2Loader stage/install/status/recovery 流程，不得通过 reset、erase 或底层 UART flash 绕过 Loader。Image identity 和 App command name 均为 `lua-cosmic-drift`；不得借用 `lua-flappybird` package、launcher 或 image identity。

运行验收先观察 `H2_LUA_COSMIC_DRIFT_READY display=368x448 touch=ft3168 button=boot`，它只会在首帧已经 present、Touch 已打开且 Lua job 进入 waiting 后输出并确认 image。Title 与 active game 各采样至少 30 秒；active game 保持 18 个 simulation bodies，日志中的平均 FPS 至少为 23，且不得出现 crash、watchdog 或 coredump。触摸应改变玩家方向；NPC 以共享方向分组漂移、相互吞食并成长；较小天体进入短距离捕获范围后向玩家加速；升级选择可增加成长、推进或可见的环绕 shield satellites。Boot/Back 必须取消 job，App 释放 framebuffer、Touch、callback、worker 和 VM，随后保持 H2Loader App command recovery 可用。
