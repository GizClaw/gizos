# Lua Cosmic Drift

`Lua Cosmic Drift` is a portable touch-first cosmic evolution game. The Lua script owns input, simulation, collisions, upgrades, particles, and rendering; the C App owns the Runtime, Lua host/job lifecycle, cancellation, and cleanup.

The Desktop target is `//projects/example/targets/cc_binary/lua-cosmic-drift:example-lua-cosmic-drift`. Click once to start, then move the pointer inside the window to steer without holding a mouse button. The AMOLED target is `//projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/amoled:package`; hold or drag on the touch surface to steer and press Boot to leave the App.

Consume cyan-ringed bodies, avoid red-ringed bodies, and choose an upgrade whenever the mass bar fills. Bodies arrive in drifting groups that share a slowly changing heading, while nearby prey flee, amber rivals strafe, and some larger red bodies hunt. Larger bodies also consume smaller bodies they collide with and grow, so the surrounding ecosystem keeps changing without player contact. Smaller bodies that enter the player's short capture range are pulled inward; Growth upgrades extend that range slightly. Hit amber rivals head-on at speed to break them before they can side-swipe you. Shield upgrades add visible orbiting satellites that absorb dangerous contact.
