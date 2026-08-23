# DevKit PAL Preference

`projects/e2e/targets/h2loader_tar_zlib/pal-pref/devkit` 把 portable PAL Preference E2E App 编译成 board `devkit`、target `esp32s3`、role `app`、image `pal-pref` 的 H2Loader package。

## 构建

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/pal-pref/devkit:package
```

构建生成 `bazel-bin/projects/e2e/targets/h2loader_tar_zlib/pal-pref/devkit/package/devkit-pal-pref-esp32s3.update.tar.zlib`。

## Layout cutover

该 App 依赖 DevKit H2Loader layout 中独立的 256 KiB `pref` LittleFS partition。普通 managed package 不写 partition table；仍使用旧 layout 的设备必须先通过明确授权的 factory recovery 安装同一提交生成的 DevKit Loader recovery bundle，再通过 H2Loader 安装本 App。Factory recovery 会擦除设备；不能把 Tiga 或其它 board 的 recovery bundle 用于 DevKit。

## 验收

每次启动只运行一个 Preference phase。`seed` 覆盖全部 PAL 类型、16 KiB blob、同值写和 1,000 次变值替换，成功后确认 App 并重启；`verify` 在重启后验证持久化数据、删除与清空，再次重启；`complete` 验证清理后的终态并持续提供 H2Loader command service。

验收必须记录每个 `H2_PAL_PREF_E2E` phase、两次真实重启、最终 `H2_PAL_PREF_E2E_READY status=PASS`，并重新执行 H2Loader `status` 确认 `board=devkit`、`target=esp32s3`、`active_role=app`、image `pal-pref` 和 `state=confirmed`。冷启动后还必须重复终态 PASS，不能只把 package 传输或首次启动当作完成。
