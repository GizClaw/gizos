# Audio System target ownership

The maintained Audio System example now owns its JieLi target under
`projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit`.
Its task policy, native wrapper, library graph, firmware and package moved together.
The five previous Display labels remain aliases for command compatibility.

The target directly depends on the shared example startup native component; a
Bazel dependency query filtered to example targets returns only this Audio System
target's labels, with no Display target dependency.

The C wrapper is byte-identical before and after the move (SHA-256
`9128f4786f54a41e3d1c4fc875699676eff00d209f44f5700a0d17c14b4f8a92`).
Task names, priorities, stack sizes, image name, App role, shared board layout and
music package data remain unchanged. No playback, microphone or AEC behavior was
changed or accepted on hardware as part of this move.

Building through the old Display `audio_system_package` alias with the native
AC791N Linux toolchain passed, including firmware and tar.zlib generation
(50.150 seconds). Local log: `tmp/jieli/audio-owner-build.log`.
