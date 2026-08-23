# DinoDive App

This standalone App hosts `projects/pixa_games/libs/dinodive` through Runtime and Game Runtime. It loads the shared Player PIXA from `/data/player/player.pixa.bin`, renders at `240x240`, and uses the Runtime audio provider for generated game effects. The `.bin` suffix preserves the original in-memory PIXA instead of asking the H2Loader installer to expand it into a `.pixa.d` tree.

Required logical controls:

- `left`: select persistent left movement.
- `right`: select persistent right movement.
- `record`: start the game; after game over, reset and immediately retry.

Target-specific Runtime setup, `component_id` to `periph_id` mapping, and
lifecycle wiring belong to the consuming launcher. The H2Loader board launcher
is one such consumer at
`projects/pixa_games/targets/h2loader_tar_zlib/dinodive/<board>/`.
