# DinoTetris App

This portable App hosts `projects/pixa_games/libs/dinotetris` through Runtime and Game
Runtime. The consuming launcher owns Runtime setup and maps left, right, and
record buttons; the game owns falling-block state, timing, rendering, score,
and result.

Record tap rotates. Holding Record enters fast drop, and releasing it restores normal drop. Record retries after game over.
