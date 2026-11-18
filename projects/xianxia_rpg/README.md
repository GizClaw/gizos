# Xianxia RPG

《竹溪仙缘》 is an original top-down xianxia role-playing game in Chinese. Version 0.3 bundles three chapters: 《竹溪灯影》, where the hero seals the ink dragon that darkened the creek; 《渡口迷雾》, where he lights lamp posts with Jiang Li to clear the river fog; and 《雨夜剑阁》, where the ferry's sword tassel leads them up a rainy mountain to rotate sword posts, read three teaching steles and face the sword spirit Hearing Rain. Like [Fruit Slice](../fruit_slice/), the portable App in [`app/`](app/) depends only on Runtime and the Display, Touch and Pref PAL contracts; it renders RGB565 in software and pushes only the dirty row bands each frame. The public contract is [`h2_xianxia_rpg.h`](app/include/h2_xianxia_rpg.h).

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

Every action is a tap or mouse click. Tap the ground to walk along the shortest path; tap a character, the well, a lamp post or a boat to walk over and interact; tap anywhere to advance dialogue. A gold diamond marks whom or what to visit next, the HUD shows HP, MP, pills and the current objective, and a banner announces every objective change. Battles offer 攻击, 剑诀 (three strikes for 7 MP, taught by the Daoist), 丹药, 防御 (softens the next blow and restores MP) and 逃跑. The chapter-1 boss telegraphs 墨浪 with a 蓄力 turn, roars at low health and then charges more often; guarding through the wave is the intended answer. Losing a battle revives the hero at the last rest point with full strength; losing to a boss also explains how to recover.

In chapter 2 the companion acts after the hero, and the bubble above her shows what she will do: she lifts her lantern to strip an enemy's veil (a veiled enemy takes 30% damage), braces a guarding hero, or strikes with her pole. Unveiled hits, her lantern and her bracing each light one of three rapport lanterns; with all three lit, 剑诀 becomes 合击, a joint strike that also breaks a charge. The boss 雾魇 veils itself, gathers a mist lock that seals sword arts for one command, and below half health splits into two shadows: after choosing a strike, tap the shadow her lantern rings. The tall reeds hide ambushes, and paths avoid them when another way exists. Lit lamp posts are rest points.

Chapter 3 continues from the chapter-2 result with its level, pills, trust and clues. Without that clear it offers a recap and Lv6, three pills, the sword art and Jiang Li. Both earlier chapters remain replayable. The west, south and east sword posts turn clockwise on each tap; point their bright tips east, north and west toward the central stele. The gate then exposes the third teaching stele. Each stele enables its matching battle hint: guard through 垂雨 to fill rapport, use the joint strike to interrupt 穿檐, and guard or take a pill during 回风 to avoid its counter. The three poses repeat without a time limit. Safe stone paths avoid all optional encounters; the default hero can win without grinding or pills. Every map has a rest point and the hall establishes one automatically before the boss.

The title offers 新的旅程 (chapter 1), 继续旅程 (the journey saved last) and 章节. The chapter screen resumes an unfinished chapter or starts it, with 从头 to restart it. Chapter 2 is always open: after a chapter-1 clear it starts with that hero, otherwise with a recap card and Lv4, three pills and the sword art. Finishing chapter 1 leads into chapter 2, and finishing chapter 2 leads into chapter 3.

## Saves

All records live in the `xianxia_rpg` Pref namespace; the Web entry keeps them in browser `localStorage`, per origin, and the desktop entry keeps them in memory only. A defeat never saves; it only revives the hero.

| Key | Format | Written |
| --- | --- | --- |
| `save0`..`save3` | Chapter-1 journey, four `u32` exactly as v0.1 writes them; `save3` holds the statistics and may be absent | Elder's briefing, Daoist's teaching, well or spring, after the boss, at the chapter-1 ending |
| `journey` | 32-byte blob: a chapter-2 journey with its rest point, trust and a fingerprint of `save0`..`save3` at the time it was written | Chapter start, briefing, companion joining, tea stall, lit lamp posts, the boat, landing, after the guard and the wraith, returning to the dock, the chapter-2 ending |
| `journey3` | The same 32-byte v2 layout for chapter 3; its fingerprint covers both older journeys | Chapter start, letter, each post rotation, each stele, rest points, hall arrival, boss victory, reveal and ending |
| `chapters` | 168-byte blob: cleared-chapter bits and the hero at each clear | Every chapter ending; once at start when only `save0`..`save3` hold a chapter-1 clear |

Chapter 3 uses its own key because released v0.2 rejects a chapter-3 journey. Its fingerprint takes precedence only while the older journeys have not changed. Rolling back to v0.2 can still open the untouched chapter-2 result; upgrading again resumes chapter 3. If the older build has saved new play, Continue follows that play and the chapter-3 row still resumes its checkpoint. The ten-slot `chapters` layout and version stay unchanged: v0.2 already decodes and preserves all ten slots. Unreadable or future-version blobs are never replaced automatically, including during legacy migration. Explicit player starts/checkpoints replace only their chapter's journey; completing a chapter replaces the chapter record. A defeat itself does not write, so consumed pills and turns remain in memory until another checkpoint, as in earlier chapters.

Continue resumes the `journey` record when its fingerprint matches the current `save0`..`save3`, and the chapter-1 record otherwise, so playing v0.1 again and returning still resumes what was saved last. The App never rewrites `save0`..`save3` for a later chapter, so a v0.1 build on the same origin keeps reading its own record. Blobs carry a checksum; a record that fails validation is ignored and left untouched, and the title says so.

## Content

Chapter 1 runs 竹溪村 → 幽篁小径 → 墨溪古祠 → back to 竹溪村; chapter 2 runs 青萍渡 → 芦苇滩 → 沙洲沉船 → back to 青萍渡; chapter 3 runs 雨中山道 → 废剑阵庭 → 剑阁正殿. Chapter 1, the chapter table and the shared UI strings live in [`h2_xianxia_rpg_story.c`](app/src/h2_xianxia_rpg_story.c); each later chapter owns one file, such as [`h2_xianxia_rpg_chapter2.c`](app/src/h2_xianxia_rpg_chapter2.c), with its maps, characters, scripts and tile rules. Maps are 15x10 character grids, scripts are lists of speaker lines with one effect when they finish, and `h2_xianxia_rpg_talk_script()` chooses what each character says at each story stage. Stages run in play order across chapters. The story tables are also the glyph sources of the body font, which the build generates from the shared [Noto Sans SC](../../libs/ui/fonts/noto_sans_sc/) font. The bundled subset has no 「」 or 『』, so text uses “” and 《》; `render_test` fails when any on-screen string has a missing glyph.

The run loop logs `H2_XIANXIA_RPG_STATE` once per phase, map, stage, battle-state, dialogue-line, story-flag or walking change, `H2_XIANXIA_RPG_RECORDS_LOAD` and one store line per written record, and `H2_XIANXIA_RPG_STATS` every ten seconds with frame time and pushed pixels.
