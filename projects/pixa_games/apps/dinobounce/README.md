# DinoBounce App

This standalone App hosts `projects/pixa_games/libs/dinobounce` through Runtime and Game Runtime. It loads the shared Player PIXA from `/data/player/player.pixa.bin`.

- `left` and `right` select the persistent paddle direction.
- `record` launches the ball.
- after game over, `record` resets the game.

Target-specific Runtime setup, component mapping, and lifecycle wiring belong to
the consuming launcher. The H2Loader board launcher is one such consumer at
`projects/pixa_games/targets/h2loader_tar_zlib/dinobounce/<board>/`.
