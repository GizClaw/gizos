# DinoDive

DinoDive 是在持续上移的平台间下落和穿行的 game library，ownership 位于 `projects/pixa_games/libs/dinodive/`。Host 负责 Runtime event 和产品流程，DinoDive 负责 horizontal direction、gravity、platform、spike、floor count 和 game result。

通用接入边界见 [PIXA Games](/apps/pixa_games)。

## Required Buttons

| Role | Gesture | Game behavior |
| --- | --- | --- |
| `left` | down | 按下时立即将持续移动方向切换为向左 |
| `right` | down | 按下时立即将持续移动方向切换为向右 |
| `action` | down | 按下时从静止的初始画面开始游戏 |

DinoDive gameplay 要求 `left`、`right` 和 `action`。这些输入在 button down 边沿立即生效，不等待后续 button click。方向输入不是只移动一步；最近一次方向输入持续生效。创建或 reset 后，scene 保持初始状态，直到收到 `action` down。Reset、退出 route 和 game-over popup 操作由 host 处理。

H106 引用 DinoDive 时，把自己的逻辑 button component 映射成 `left`、`right` 和 `action`。`action` 可以复用其他 game 中承担 jump/start 的同一个 host component；这些 game role 仍由各自 game 定义。DinoDive 不知道 H106 component ID 或 board `periph_id`。

## Gameplay

- Player 初始站在第一个无 spike 的 platform，并保持静止；收到 `action` down 后默认向右移动。
- Horizontal movement 持续进行，player 离开一侧 screen 后从另一侧 wrap。
- Gravity 使 player 下落；只有从上方向下落到 platform 时可以 landing。
- Platform 持续向上 scroll，离开顶部后在底部重新生成。
- 新 platform 在允许范围内随机选择 horizontal position，并可以生成 spike。
- Platform 越过顶部并重新生成时 floor count 增加。
- Scroll speed 随 floor count 分段提高，并受最大速度限制。
- Player 接触 spike，或完全离开 screen 顶部/底部时 game over。

Legacy H106 的 `dinodive.c` 是玩法和视觉节奏参考，不是需要逐行翻译的 physics implementation。PixelRoot 版本保持 `240x240` play area、自动横向移动、方向切换、向上滚动的单向平台、下落选点、水平 wrap、spike、floor count 和 game-over 规则；不能继续用整数 `x/y/velocity_y` 和手写 crossing 判断模拟物理。

Scene 使用 PixelRoot physics module：

- Player 是 `RigidActor`，sprite 为 `60x60`，独立 feet/body AABB 的底部位于 sprite `y + 58`。
- Platform 是带向上速度的 Kinematic one-way body，固定为 6 个 `80x12` platform，纵向间距为 `65 px`。
- Spike 是跟随 platform 的 Kinematic sensor，visual 为 `40x11`，sensor 四边保留 `2 px` 容差。
- Flat Solver 以固定 `60 Hz` 执行 gravity integration、one-way contact、velocity response、penetration resolution 和 sensor callback；game loop 帧率不能改变运动速度。
- Floor contact 由 PixelRoot collision callback 确认。确认后 feet 对齐 platform top，并按该 platform 的 Kinematic motion carry；离开 contact 后产生一次 `fall` event。

当前 gameplay tuning 使用物理单位：水平速度 `84 px/s`，重力 `260 px/s²`，最大下落速度 `150 px/s`。Platform scroll 从 `28 px/s` 开始，每 10 层增加 `6 px/s`，最大 `64 px/s`。这些参数用于复现旧版的落点控制节奏，不等价于复制 legacy `4 px/tick`、`gravity +1/tick` 和 `2 px/tick` scroll；调整时必须同时验证 Desktop 与 device fixed-step 结果。

第一个 platform 不带 spike；其余 platform 以 deterministic seed 生成 `x`、30% spike 概率和左右半区。Platform 越过 `y < -12` 后在最低 platform 下方 `65 px` respawn，并至少位于 `y = 240`。Platform count、gap、physics tuning、wrap rule、spike probability 和 sensor tolerance 都属于 DinoDive library。

## Visual Assets

DinoDive 运行时只接收 PIXA Games shared `player.pixa`。Host 负责提供路径、加载 asset 并保证其生命周期；DinoDive public config 不接收 environment asset 或文件路径。

| PIXA | Provider | Required clips | Contract |
| --- | --- | --- | --- |
| `player.pixa` | Host 提供并由 PIXA Games 共用 | `run_left`、`run_right` | 两个 run clip 直接来自旧 H106 Ferus GIF，均为 `60x60`、4 frame、每 frame `130 ms` 的 loop animation。DinoDive game over 沿用当前方向的 run frame，产品 popup 由 Host 负责。 |

旧 Tiga `240x240` `img_bg.png` 直接生成代码内 ARGB4444 background。`80x12` platform 按 legacy LVGL 白到青色 gradient、spread 和 shadow blur 离线栅格化为带透明 glow 边距的代码内 ARGB4444，不使用旧仓库中未被 DinoDive runtime 引用的 platform PNG。`40x11` spike 同样从旧 Tiga PNG 直接生成，保留半透明抗锯齿边缘。三者由 PixelRoot blit，只有 level text 和 debug overlay 使用 primitive。

DinoDive text catalog 包含 `floor` 和 `press_start`。Floor label 与动态数字分段交给注入的 UTF-8 provider 绘制；H2Loader 和 Desktop 使用内置 5 × 7 provider 与 English catalog。

仓库中的测试 Player PIXA 与 embedded background、platform、spike include 是已审核并提交的 game inputs，build 只消费这些 committed bytes。旧 legacy H106 checkout 和一次性 `tools/generate_dinodive_assets.py` 已不再是可复现的 source contract，因此不保留公开 converter 入口；修改 fixture 必须提交实际 source/output、说明来源并在 review 中逐字节审查，不能用手绘 placeholder 替换。

## Events and Result

DinoDive 输出：

- `fall`：player 从 platform 开始下落时产生。
- `game_over`：spike collision 或离开 vertical play area 时产生。
- Final floor count。

`fall` 同一 tick 提交约 `560 ms` 的上升电子 sweep，`game_over` 只提交一次约 `1.22 s` 的分段下降音列。音效由 DinoDive 的 integer Hz/ms/permille recipe 定义，shared `h2_game_audio` 内嵌 PixelRoot `DefaultAudioScheduler` 和 `ApuCore`，在运行时生成 `16 kHz` mono S16LE PCM，再通过 PAL audio track 输出；scheduler storage 跟随 PAL 分配的 game audio object，不产生隐藏 heap allocation。仓库不保存 PCM、WAV 或 Opus；legacy `player_jumps.opus` 和 `fall_platform.opus` 只用于首次本地 A/B。

Host 在创建 `h2_game_audio` 前启动 speaker，并在销毁 game audio 后决定是否停止共享 speaker。DinoDive event callback 是同步 observer；callback 内不能 destroy、reset 或重入当前 game，只能记录 event 并把产品 effect 留给 host main loop 执行。

H106 根据 final floor count 决定 XP、reward 和结果页面；DinoDive 不访问 H106 pet state。

## Acceptance

创建和 reset 后，gameplay state 在收到 `action` down 前不能推进。开始后，相同 platform seed 和 left/right 输入序列下，PixelRoot 版本必须保持 direction、screen wrap、platform respawn、spike placement、landing、floor count、scroll progression、game-over condition 和 event ordering 一致。测试必须证明 Player、Platform 和 Spike 分别使用 Rigid、Kinematic one-way 和 sensor body，并证明不同 render tick 分组得到相同 fixed-step state。
