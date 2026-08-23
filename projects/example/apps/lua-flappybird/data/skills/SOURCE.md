# Flappy Bird Skill Source

- Repository: <https://github.com/espressif/esp-claw-skills-lab>
- Upstream revision: `9c1ab0fb24655a44556abca962d6109876a57e13`
- Upstream path: `skills/flappybird/`
- Original script SHA-256:
  `cb5470e7e41516ab83b5f910aeb539aa3235ed58b62b1da648fc812d51d2429c`
- Original SKILL.md SHA-256:
  `5e88701815ee34a1c4211ebee1d35821c038a3240728e87d32288176a016d1e5`
- License: MIT; the upstream license is retained in `../LICENSE`.

The repository does not vendor a second copy of the original Skill. The source
URL, revision, path, hashes, and license above identify the migration input.

The migrated script keeps the upstream `display`, `lcd_touch`, and `audio`
module contracts. Those modules call the singleton Runtime PAL APIs directly.
Only physical Button acquisition changes to a numeric
`runtime.components.get()` lookup. Gameplay constants, tones, rendering, state
machine, physics, collision, scoring, best score, restart, diagnostics, frame
loop, and cleanup behavior remain in the migrated source.
