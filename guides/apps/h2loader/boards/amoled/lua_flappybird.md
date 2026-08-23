# Lua Flappy Bird on AMOLED

该 H2Loader App 使用 AMOLED 368 x 448 Display、Touch、Boot button 和 PSRAM
Memory provider，运行与 Desktop/Browser 相同的 portable App 和 Lua bytes。
Boot button 映射到 App 的 Back component；Touch 负责 flap 和碰撞后的 restart。

构建：

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-flappybird/amoled:package
```

设备已经可通过 H2Loader 通信时，只能按 [AMOLED H2Loader](./h2loader) 使用
H2Loader stage/install/status/recovery 流程，不得通过 reset、erase 或底层 UART
flash 绕过 Loader。

验收要求：首帧已经 present、Touch 已打开且 Lua job 进入等待后才确认 image；
观察 flap、pipe movement、collision、score、restart；Boot/Back 在期限内把 job
置为 cancelled，App 释放 framebuffer、Touch、Timer、callback、worker 和 VM。
