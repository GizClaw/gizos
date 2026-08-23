# Showcase 资源与持久化

本文定义 Showcase 的 SD 卡 MP4 catalog、角色 catalog、文档与运行时素材 ownership、preference 和 fallback。页面行为见[对话](/apps/showcase/conversation)和[触屏控制台](/apps/showcase/console)。

## MP4 catalog

Linux integration 把 SD 卡允许目录挂载为只读 catalog root。Scanner 生成带 `catalog_generation` 的 immutable snapshot：

| 字段 | 合同 |
| --- | --- |
| `video_id` | 基于规范化相对路径和内容 identity 的稳定 id，不使用显示名称；同时标识视频及其绑定音轨 |
| `relative_path` | 相对 catalog root，禁止 `..`、绝对路径和 symlink escape |
| `display_name` | 控制台名称列表显示使用，不作为 identity |
| `media_info` | 容器、codec、分辨率、时长和可播放校验结果 |
| `enabled` | 只有普通、可读且 media service 支持的 MP4 才为 true |

排序固定为规范化文件名的 Unicode code point 升序。控制台不提供筛选，只按该顺序列出 `enabled=true` 项；列表超过 viewport 时纵向滚动。

## Character catalog

角色 catalog 是可扩展的 Showcase-owned manifest。每项至少包含稳定 `character_id`、显示名称、对话角色图和 voice binding；控制台只读取 `character_id` 与显示名称，不加载或显示角色图。首批角色从 H106 的真实产品素材导入：

| `character_id` | 显示名称 | H106 制作输入 | Voice binding |
| --- | --- | --- | --- |
| `tiga` | 迪迦 | `projects/h106/assets/raw/h106/tiga/images/img_ultraman.png` 导入副本 | Showcase-owned Tiga voice alias |
| `zero` | 赛罗 | `projects/h106/assets/raw/h106/zero/images/img_ultraman.png` 导入副本 | Showcase-owned Zero voice alias |

H106 资源只作为已经确认的制作输入。Showcase 发布前必须把真实角色图和 voice binding 导入 `projects/showcase/assets/` 自己的 manifest；运行时禁止读取 `projects/h106/**` 或 H106 Bundle path。新增 H106 角色时追加 catalog entry，不改变控制台布局；控制台不为角色生成预览副本。

控制台中文使用 Showcase-owned `projects/showcase/assets/fonts/NotoSansSC-Bold.ttf`。该文件从 H106 已确认的完整中文字体源导入，运行时通过 TinyTTF 的独立 128-entry glyph cache 渲染；不能使用只覆盖 Desktop 状态页字符的字体子集，也不能在运行时读取 H106 字体路径。

对话页面原型使用的迪迦图片副本位于 `guides/public/showcase/tiga.png`，来源为 `projects/h106/assets/raw/h106/tiga/images/img_ultraman.png`。文档副本不进入运行时 package，也不显示在控制台角色列表中。

## Preference

Showcase 使用独立 namespace 原子保存：

| Key | 值 | 写入时机 | 缺失或损坏 |
| --- | --- | --- | --- |
| `showcase.video_id` | 当前稳定 MP4 与绑定音轨 id | 视频 Tab 点击确定且校验成功 | 选择内置 fallback |
| `showcase.character_id` | 当前 catalog 中有效的稳定角色 id | 角色 Tab 点击确定且校验成功 | 使用 `tiga`；缺失时使用第一个有效角色 |

两个 key 可以独立更新，但每次写入都必须先校验目标 id。Focus、scroll、draft、gesture count、conversation phase 和 error 不持久化。

## Generation 与恢复

- 每次 mount/unmount/rescan 递增 `catalog_generation`。
- 每次切换视频或 fallback 递增 `media_generation`。
- Save command 携带打开/选择时的 catalog generation；不匹配时拒绝并刷新列表。
- 当前视频消失时不删除 preference，运行时使用 fallback 并保留错误；文件恢复后下一次 scan 可以重新采用该 id。
- 进程启动先读取 preference，再用最新 catalog 校验；不能先启动一个无效路径的 decoder。

## Media package 边界

Board Spec 不猜测 codec。Showcase 发布配置必须另行规定允许的 MP4 container、video/audio codec、最大分辨率、bitrate、frame rate 和音轨策略；scanner 与 decoder 使用同一份 media policy，不能出现控制台可选但 decoder 拒绝的文件。

内置 fallback 属于 Showcase package，不位于可移除 SD 卡中。Fallback 同时提供可循环的 H.264/AAC MP4 和从同一 timeline 导出的 16 kHz mono PCM；Video Decoder PAL 驱动画面，Audio PAL 驱动声音，两个 loop 使用相同时长并同时启动。素材与授权说明位于 `projects/showcase/assets/README.md`。

## 验收

- Scanner 拒绝目录穿越、symlink escape、device node、不可读文件和不支持的 media。
- MP4 排序、角色 manifest 顺序和有效项结果在 Desktop 与 K4B 一致。
- 控制台只保存稳定 id，不从文件显示名或角色中文名反推 identity。
- SD 卡移除时使用内置 fallback，控制台和进程保持运行。
- 迟到 catalog/save/media result 通过 generation 丢弃。
- Preference 损坏时进入明确 fallback，不崩溃也不读取 H106 preference。
- Showcase runtime package 不依赖 H106 private asset path。
- 迪迦、赛罗和后续角色的真实角色图与 voice binding 都由 Showcase-owned manifest 发布，控制台只显示名称。
