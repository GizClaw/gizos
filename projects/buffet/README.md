# Tidewheel Sushi

Buy a ticket at 潮轮寿司, follow the passing plates around a cypress counter, and choose when to take a bite. Each visit lasts two minutes and thirty seconds. Choose between covering the ticket price and tasting all six dishes.

## Play

Run either target from the GizOS checkout. The browser version uses the Web PAL and the desktop version opens an SDL3 window. Both run the same C App.

```sh
bazel run -c opt //projects/buffet/targets/pkg_tar/web:serve -- --port 8792
# Open http://127.0.0.1:8792/

bazel run -c opt //projects/buffet/targets/cc_binary/desktop:buffet
```

The ticket opens in Simplified Chinese. Tap **EN** for English. Tap one of the six plates to see the food, value and fullness. Tap the goal to change it, then buy the ticket. Tap the floor to walk. A tap on a distant plate leads you to its side of the belt. Within reach, tap its lit rim or **Eat plate**. Other diners can hold up a route. Try the other end or wait for a gap. Empty plates ride to the kitchen pass and keep their original glaze.

A keyboard also works. Use **Left/Right** to browse plates on the ticket, **G** to choose a goal, **L** for language, and **Enter** or **Space** to buy. Walk with the **arrow keys** and eat with **Space**. The result offers **Play again**. **Escape** closes the desktop window. The browser fits its landscape frame to the available display and maps touch coordinates with the same transform.

## The bill

Every meal comes with an 80-coin purse. Admission costs 48. Food contributes to the meal's value and the tasting count. The bill shows food value, ticket price, their difference, any hospital fee and remaining coins. Starting another meal issues a fresh purse. The App keeps the meal and language in memory.

Capacity is 100%. Every dish digests at two percentage points per second. Above 120%, the player has seven continuous seconds to get back to the line. Reaching 120% clears that timer. Sustained red during service ends the meal immediately and deducts 24 coins. Closing has priority at the final instant. Tune the shared rules and individual servings in [buffet_game.c](app/src/buffet_game.c).

## Achievement cards

The book on the ticket opens the collection. After settlement a new print flips and slides into **This Round's Cards**. Tap **Collect cards** to finish the whole reveal immediately. Open the result's rack to browse this meal, or switch to the full collection. A browser with reduced motion enabled shows the settled rack directly. **C** opens the album, **G** changes its result filter, the arrow keys select a print and **Enter** returns.

A successful serving followed by a normal close grants `base-bite` (First Bite). Closing a paid round grants `base-settle` (Made It to Closing). A hospital ending grants the hidden `story-hospital` (First Trip to the Hospital). The album stores each catalog ID once under `buffet_cards_v1` in the Pref PAL. Desktop uses its own SQLite database in `TidewheelSushi` under Application Support on macOS, or `gizclaw/tidewheel-sushi` under the Linux data directory. `H2_DESKTOP_STORAGE_ROOT` selects an isolated absolute directory. Web uses origin-local browser storage. On a storage failure the cards remain available in the current session, the album reports that status and pending writes retry without replacing unreadable records.

## Music and sound

Buying a ticket starts **Lanterns on the Belt**, an original 112.5 BPM, 12-bar loop. The warm mallet theme, plucked accompaniment, reed voice, bass and brushes are synthesized for this shop. The 25.6-second, 16 kHz mono loop and eight short cues are linked into the App. One native S16 Audio PAL track mixes music with up to three effect voices. Voices for plate contact and the bite share the same successful serving event. The bite follows 185 ms later as the morsel reaches the mouth. Red and recovery cues follow threshold crossings. A new achievement turns with a paper sound, then a short reveal plays when its face appears. Normal closing fades the music and plays the closing cadence.

Click the small speaker or press **M**. The setting is saved in the separate `buffet_audio/muted_v1` Pref key. Muting flushes pending playback. The next round starts at the opening phrase. Backgrounding stops the speaker. The desktop entry uses the PortAudio provider with a real output device. The Web PAL opens Web Audio after a pointer or keyboard gesture. An unavailable speaker leaves the meal playable.

The original score, synthesizer, export script and delivered mono WAVs are in [app/audio/](app/audio/). `render_sushi.py` renders 24 kHz masters and also accepts `--compact-only`. `package_sushi.py` resamples the seamless loop with wrapped padding and exports the 16 kHz bank. `pack_audio.py` embeds the checked-in samples in the normal Bazel build using the Python standard library.

## Rendering and assets

The App composes an 800×480 logical stage into a 1600×960 native RGB565 framebuffer, capped at 30 fps. The SDL3 launcher requests a high-density drawable in an 800×480 window. It reports the original window units through Touch. The Web PAL uses the same 1600×960 texture and maps pointer coordinates from the aspect-fitted canvas. Static ground, working frame and last-presented frame use approximately 8.79 MiB combined. The evening room painting is registered to the actual track and encoded directly to dithered RGB565. Diners move behind and in front of the counter's silhouette. Indexed character, food and print art lives in read-only storage. Frames allocate no textures. Stationary ticket and receipt views need no display upload. A moving scene uses at most one changed band of rows per frame.

The plate images are original offline ray-marched sculptures. The six servings use a shared ceramic profile and light, with individual glazes, fish cuts, rice, seaweed, egg folds, prawn tails, cucumber cores and tofu pockets. A second bake preserves their shapes at room scale. Each human diner has a registered character sheet. The blue-haired visitor uses separately drawn front, three-quarter, profile, rear, bite and savor poses. Her hair follows the stride and her feet share a sole anchor with the turn poses. The source lives in [art/](app/art/) and the baked indexed images in [plates/](app/plates/). The game build uses the committed plates. To rebake them, use Python with Pillow 11.3 and CairoSVG 2.8, Cairo, and a host C compiler.

```sh
python3 projects/buffet/app/art/bake.py
```

The project font converter builds four anti-aliased strikes from the shared Noto Sans SC Bold for Chinese and Barlow Semi Condensed SemiBold for English. Each language falls back to the other's glyphs. The original Barlow font comes from [Google Fonts](https://github.com/google/fonts/tree/main/ofl/barlowsemicondensed). The font licenses are in [app/fonts/](app/fonts/).

```sh
bazel test //projects/buffet/app:all //pal/providers/backends/sdl3:sdl3_test
bazel build -c opt //projects/buffet/targets/pkg_tar/web:buffet
```

The web artifact `buffet.web.tar` carries `index.html`, `index.js`, `index.wasm` and `_headers`. The preview server applies the required cross-origin isolation headers for Emscripten pthreads.
