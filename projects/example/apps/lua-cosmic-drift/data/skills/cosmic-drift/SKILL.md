---
{
  "name": "cosmic-drift",
  "description": "Run a touch-first cosmic evolution game on the Runtime display.",
  "author": "GizOS contributors",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["cosmic", "evolution", "touch", "lua"],
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

# Cosmic Drift

On Desktop, click once to start and move the pointer to steer without holding a mouse button. On a touch display, hold or drag to accelerate toward the touch. Consume bodies with cyan rings, use caution around amber rings, and avoid red rings. Bodies enter in directional groups; prey can flee, rivals can strafe, and larger hunters can override the shared drift when a target is nearby. Non-player bodies consume smaller bodies and grow. Smaller bodies inside the player's short capture range are pulled inward. Build speed and ram amber rivals head-on to break them. Choose one of three persistent upgrades whenever the mass bar fills; shield upgrades add orbiting satellites.

## Requirements

- Runtime Display singleton.
- Runtime Touch singleton.
