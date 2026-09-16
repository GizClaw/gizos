# MP4 target ownership

The AC791N MP4 entry now belongs to
`projects/example/targets/h2loader_tar_zlib/mp4-player/jieli_ac791n_devkit`.
Its native launcher, direct-boot override, library graph, task policy and
small/large media packages move together. The two C files are unchanged;
package image names, board/target identity, role, media labels and media
roots remain unchanged. The old Display labels remain compatibility aliases.

The App power regression now reads the MP4 source from its new owner;
both compiled C regression cases passed. A dependency query for the new
small package found no dependencies on Display targets.

This is an ownership change, not a playback, audio, or Loader acceptance
claim. No MP4 package was installed during this pass. Audio/Touch/Button
entry separation and full PAL/diagnostic review remain separate work.
