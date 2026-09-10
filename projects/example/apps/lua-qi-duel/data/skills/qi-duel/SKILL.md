---
{
  "name": "qi-duel",
  "description": "Play Qi Duel with touch controls, a computer opponent or a nearby peer.",
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
directional edge fades. The game resolves simultaneous rounds with qi, health, guards and combo waves.
Click the opening particle field to search for a peer; an unsuccessful search
continues into a computer match. Background music and skill cues use a streaming
retro synthesizer.

## Requirements

- Runtime Display singleton.
- Runtime Touch singleton.
