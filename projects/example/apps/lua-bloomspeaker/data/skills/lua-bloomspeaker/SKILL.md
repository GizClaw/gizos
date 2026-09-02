---
name: lua-bloomspeaker
description: Run the portable microphone-reactive BloomSpeaker particle intercom UI demo.
---

# BloomSpeaker

Run `scripts/main.lua` as the single app scene. The script owns long-press
gesture recognition, level smoothing, animation, and rendering. The native
host owns Runtime, the intercom controller, and the Lua job lifecycle.
