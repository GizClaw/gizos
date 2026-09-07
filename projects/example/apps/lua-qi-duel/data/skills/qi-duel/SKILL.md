---
{
  "name": "qi-duel",
  "description": "Preview the touch-first Qi Duel combat interface.",
  "author": "GizOS contributors",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["qi-duel", "touch", "lua", "amoled"],
    "peripherals": ["display", "touch"],
    "cap_groups": ["cap_lua"],
    "manage_mode": "web"
  },
  "simulator": {
    "entry": "scripts/main.lua",
    "files": ["scripts/main.lua"]
  }
}
---

# Qi Duel

Swipe left or right across the lower carousel to rotate between Charge, Wave,
Absorb, and Guard. The selected skill is larger and opaque; adjacent skills use
directional edge fades. This prototype does not resolve combat yet; it exercises
the AMOLED composition, touch path, continuous idle motion, meter transitions,
and frame-rate telemetry.

## Requirements

- Runtime Display singleton.
- Runtime Touch singleton.
