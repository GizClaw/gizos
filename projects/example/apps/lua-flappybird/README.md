# Lua Flappy Bird

This portable Example starts exactly one compiled Lua Skill. It has no gallery
or selection UI. The same App consumes Runtime events, directs allowed events
to the explicit Lua job, and owns cancellation and cleanup.

The committed game is the complete migration of the pinned ESP-Claw Skill. It
keeps the upstream Display, Touch, and Audio modules, backed directly by the
Runtime singleton PAL APIs. Only physical Button discovery uses
`runtime.components.get(component_id)`. The portable App consumes the Runtime
event queue once and forwards only selected copied events to the job.

See `data/skills/SOURCE.md` for the upstream URL, revision, hashes, and license.
