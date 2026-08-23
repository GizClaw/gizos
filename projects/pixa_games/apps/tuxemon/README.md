# Tuxemon App

This standalone App hosts the portable Tuxemon prototype through Runtime and
Game Runtime. It loads the shared Player PIXA from
`/data/player/player.pixa.bin`.

- `ok` moves up.
- `back` moves down.
- `left` and `right` move horizontally.
- button down and up edges are forwarded so holding a button keeps moving.

Target-specific Runtime setup, component mapping, and lifecycle wiring belong to
the consuming launcher. The H2Loader board launcher is one such consumer at
`projects/pixa_games/targets/h2loader_tar_zlib/tuxemon/<board>/`.
