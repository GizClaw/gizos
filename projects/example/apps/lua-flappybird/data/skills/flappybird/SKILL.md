---
{
  "name": "flappybird",
  "description": "Run a Flappy Bird mini-game on the board LCD. Prefer LCD touch; otherwise use a GPIO button. Optional audio on audio_dac.",
  "author": "ESP-Claw contributor",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["flappybird", "arcade", "demo", "button", "touch"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"],
    "manage_mode": "web"
  },
  "simulator": {
    "entry": "scripts/main.lua",
    "files": [
      "scripts/main.lua"
    ]
  }
}
---

# Flappy Bird

Use this skill when the user asks to play a game, run Flappy Bird, start a
bird game, or launch an interactive game demo on the board.

The Lua script renders the bird, pipes, and score on the LCD and reads input
events to make the bird flap.

## Requirements

- Runtime supplies the singleton Display API.
- Runtime Touch is preferred; if it is unavailable the script falls back to an
  existing Runtime Button component.
- Runtime Audio System is optional and used only for sound effects.

## Tool Call Inputs

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/main.lua",
  "args": {
    "button_component_id": 3
  }
}
```

The Button component ID is launcher-owned. Display, Touch, and Audio are
singleton Runtime capabilities and do not use component IDs. Button is required
when Touch is unavailable.

Common optional args:

| Arg | Default | Meaning |
|-----|---------|---------|
| `sample_rate_hz` | `16000` | Output sample rate |
| `channels` | `1` | Output channel count |
| `bits` | `16` | PCM sample width |

## Behavior

Each tap or button press makes the bird flap upward. Hitting a pipe or the
ground ends the round. The game loop runs until the runtime stops it. On startup
or runtime failure, report the `[lappybird] ...`
error line directly to the user and do not retry with changed arguments unless
the user asks.

## Files

- `scripts/main.lua`
