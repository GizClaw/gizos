# Tidewheel Sushi

Buy a ticket, watch for a dish you like, and choose where to eat before the shop closes. A meal lasts two minutes and thirty seconds. The Wave and Sun conveyors travel in opposite directions. A raised rear pass serves more sushi. The west counter serves shoyu ramen and the east counter serves chawanmushi.

## Play in a browser

```sh
bazel run -c opt //projects/buffet/targets/pkg_tar/web:serve -- --port 8792
```

Open `http://127.0.0.1:8792/`. The ticket opens in Simplified Chinese. Tap **EN** to switch to English. Choose a dish thumbnail for its name, value and portion size. Tap the meal goal to choose between covering the ticket price and tasting all six sushi. Both side dishes contribute to value and fullness.

Drag the lower-left stick to walk. A reachable serving gains a bright rim and enables the bowl button at lower right. On single-touch devices, lift the moving finger and press the bowl once to eat. The browser also accepts a second finger on the bowl while the stick is held. Use **Left/Right** on the menu, **L** for language, **G** for the goal and **Enter** or **Space** to buy. Walk with the **arrow keys** and eat a reachable dish with **Space**. The browser letterboxes the canvas and maps touch coordinates into the game stage. Backgrounding pauses the meal.

The upper-left serving slip shows a dish, its counter crest and the seconds until it arrives. When a portion has eight seconds left on the counter, an hourglass ticket takes its place. Up to three tickets list the soonest withdrawals. During the final three seconds the ticket warms to red and the dish gently pulses inside fixed corner marks. At its deadline the kitchen clears the setting for service. The ticket menu lists each dish’s shelf time. Two loops hold eight carriers each. The rear pass has three settings and each side counter has four. A full row waits for a vacancy before scheduling its next dish. The bottom-center plate count and rice bowl track the meal. A white notch marks 100% full. Pointed segments mark danger. The clock beside the speaker counts down to closing.

The menu just left of the closing clock pauses the meal. Its paper slip shows time left, plates eaten, food value against admission, and fullness. In the red zone it also shows the hospital countdown. **Continue** resumes the same instant. Choose **Leave meal** and confirm to return to the ticket counter. The counter shows the last purse after admission and the next meal's starting purse. Closing cards are awarded for completed rounds. On the Web, **Escape** opens the menu, resumes from the status sheet or backs out of the exit question. **Left/Right** and **Enter** select a sheet action.

## Food and the bill

The meal purse holds 80 coins and admission costs 48. A small bowl of shoyu ramen contributes 24 coins and 32% fullness. A steamed custard contributes 9 coins and 9%. Digestion removes two percentage points per second. Seven continuous seconds above 120% ends the meal and deducts 24 coins. Reaching the safe line clears the danger timer. Closing at the final instant gives the normal receipt. The receipt compares food value with admission and shows the remaining purse. **Play again** issues a new meal purse.

Stage prices and digestion are defined in `app/src/buffet_game.c`. Carrier counts, serving intervals and meal order are in `app/src/buffet_stations.c`.

## Achievement cards

The book on the ticket opens the collection. A normal close after eating grants `base-bite` and a paid round that reaches closing grants `base-settle`. A hospital ending grants the hidden `story-hospital`. Each new print turns from back to edge to face and slides into the result rack. **Collect cards** completes the sequence immediately. Tap a print in the settled result rack or an owned print in the album to lift it into view. **Back** returns to its source. Reduced motion shows the settled print immediately. **C** opens or backs out of the album, **G** changes its result filter, arrow keys select a print and **Enter** opens or closes the selected print.

The Pref PAL stores one record per catalog ID under `buffet_cards_v1`. The Web App serializes first awards with a browser lock across tabs. The receipt reports a local save failure and retries pending writes. Pref data belongs to the browser origin.

## Rendering, art and sound

The 20×13 world projects onto an 800×480 logical stage. The Web PAL presents a 1600×960 RGB565 backing surface. The room uses original beveled, vertex-colored solids. Joined cabinets, treads, vessels and island edges render with a per-pixel height buffer when entering the room. Every dynamic belt seam and moving plate uses its lane's arc length. Registered feet, table footprints and access points share this projection. Four facings, walk, reach, bite and savor poses share a sole anchor.

The eight dishes have separate ceramic, ingredient, shadow and held-bite artwork in `app/art/service/stations/`. The game uses the registered sprite outputs in `app/plates/`. The optional art build uses Pillow 11.3 and CairoSVG 2.8. To regenerate the illustrations, run `python3 projects/buffet/app/art/bake.py`. Noto Sans SC Bold and Barlow Semi Condensed SemiBold provide four strikes with cross-family glyph fallback. Font notices accompany the Web archive.

Buying a ticket starts *Lanterns on the Belt*, an original 25.6-second loop. A soft bowl strike announces fresh stock. The original music and cues live in `app/audio/`. A successful pickup sounds the plate, then the bite 185 ms later. Warnings, recovery, card flips, reveals and closing have distinct sounds. The speaker or **M** toggles `buffet_audio/muted_v1` in Pref. Web Audio opens after the first gesture.

The Web artifact `buffet.web.tar` contains the game, fonts and cross-origin isolation headers for Emscripten pthreads. Use the supplied archive server for local play.
