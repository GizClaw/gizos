---
name: lua-bloomspeaker
description: Run the portable microphone-reactive BloomSpeaker particle intercom UI demo.
---

# BloomSpeaker

Run `scripts/main.lua` as the single app scene. The script owns long-press gesture recognition, level smoothing, animation, transitions, and rendering. The native App host owns the intercom controller and Lua job lifecycle; platform targets only compose Runtime providers and board mappings.
