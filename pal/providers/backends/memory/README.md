# Volatile Pref provider

`memory_pref` implements the public Pref PAL using an injected Memory PAL. It has no platform, filesystem, board or application dependency. Callers serialize operations and choose explicit namespace, entry, value-size and handle limits. Readers see committed values; one writer per namespace has private staging. Commit atomically replaces the RAM state, including allocation-failure rollback. Closing a writer discards uncommitted edits. Iterators snapshot metadata only and must close before their namespace. Values and staging copies are wiped on replacement or destruction.

This provider is intentionally volatile. Commit means visibility within the provider lifetime, without filesystem writes or reset durability. Provider destruction requires all handles to be closed; recreating it starts empty. See [the public contract](include/h2_memory_pref.h) and run `:pref_test` for transactions, ownership, bounds, failure cleanup and recreation behavior.
