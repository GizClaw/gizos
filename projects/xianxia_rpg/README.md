# Xianxia RPG

《竹溪仙缘》 is an original top-down xianxia role-playing game in Chinese. Version 0.1 ships the complete first chapter, 《竹溪灯影》: explore three scenes, talk to villagers, fight turn-based battles and seal the ink dragon that darkened the creek. Like [Fruit Slice](../fruit_slice/), the portable App in [`app/`](app/) depends only on Runtime and the Display, Touch and Pref PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_xianxia_rpg.h`](app/include/h2_xianxia_rpg.h).

| Entry | Platform | Package |
| --- | --- | --- |
| [`targets/pkg_tar/web`](targets/pkg_tar/web/) | Browser (WebAssembly), 640x480 | `:xianxia_rpg`, `:serve` |
| [`targets/cc_binary/desktop`](targets/cc_binary/desktop/) | macOS/Linux SDL3 window | `:xianxia_rpg [WIDTHxHEIGHT]` |

```sh
bazel test //projects/xianxia_rpg/app:all
bazel build -c opt //projects/xianxia_rpg/targets/pkg_tar/web:xianxia_rpg
bazel run -c opt //projects/xianxia_rpg/targets/pkg_tar/web:serve -- --port 8791
bazel run //projects/xianxia_rpg/targets/cc_binary/desktop:xianxia_rpg
```

The Web archive `xianxia-rpg.web.tar` contains `index.html`, `index.js`, `index.wasm` and `_headers`. A static host must send `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`, because the App runs on a pthread.

## Playing

Every action is a tap or mouse click. Tap the ground to walk along the shortest path; tap a character, the well or the spirit spring to walk over and interact; tap anywhere to advance dialogue. A gold diamond marks whom to visit next, the HUD shows HP, MP, pills and the current objective, and a banner announces every objective change. Battles offer 攻击, 剑诀 (three strikes for 7 MP, taught by the Daoist), 丹药, 防御 (softens the next blow and restores MP) and 逃跑. The boss telegraphs 墨浪 with a 蓄力 turn, roars at low health and then charges more often; guarding through the wave is the intended answer. The shrine objective recommends level 3, which takes a few forest or shrine battles. Losing a battle revives the hero at the last well or spring with full strength; losing to the boss also explains how to train, guard and recover.

The journey is saved after the elder's briefing, after the Daoist's teaching, when resting at the village well or the shrine spring, after the boss is sealed and when the chapter ends. It is stored as four `u32` keys, `save0` to `save3`, in the `xianxia_rpg` Pref namespace, and the title then offers 继续旅程. `save3` holds the ending statistics; saves without it load with zero statistics. Continuing a finished chapter reopens its ending with the saved level, battles and turns instead of the boss fight. A defeat never saves; it only revives the hero. The Web entry keeps the keys in browser `localStorage`; the desktop entry keeps them in memory only.

## Content

The chapter runs 竹溪村 → 幽篁小径 → 墨溪古祠 → back to 竹溪村. Maps, exits, encounter tables, characters, dialogue scripts and UI strings live in [`h2_xianxia_rpg_story.c`](app/src/h2_xianxia_rpg_story.c): maps are 15x10 character grids, scripts are lists of speaker lines with one effect when they finish, and `h2_xianxia_rpg_talk_script()` chooses what each character says at each story stage. The story table is also the glyph source of the body font, which the build generates from the shared [Noto Sans SC](../../libs/ui/fonts/noto_sans_sc/) font. The bundled subset has no 「」 or 『』, so text uses “” and 《》; `render_test` fails when any on-screen string has a missing glyph.

The run loop logs `H2_XIANXIA_RPG_STATE` once per phase, map, stage, battle-state or dialogue-line change, and `H2_XIANXIA_RPG_STATS` every ten seconds with frame time and pushed pixels.
