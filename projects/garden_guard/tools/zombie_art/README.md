# Zombie sprite sources

These Garden Guard characters were drawn for this project in `art.py` from original path art. The source and generated sprites follow the GizOS project license. The project owns the drawings, costumes, motion poses, and palettes.

The runtime include contains 64×72 world cels with a common foot point at `(32, 64)` and a 64×64 journal portrait for each character. Each row stores its first visible column, its length, and the packed pixels. Pixel bytes hold 2-bit alpha and a 6-bit palette index. The table uses half-open bounds for incremental painting. The renderer draws ground shadows and selects cels using gameplay clocks.

Run the generator with Python, Cairo, and the versions in `requirements.txt`. The generated include is checked in so firmware builds require only the existing C toolchain. Example from the repository root:

```sh
python projects/garden_guard/tools/zombie_art/generate.py \
  projects/garden_guard/app/src/h2_garden_guard_zombies.inc
```
