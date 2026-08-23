# MP4 Decoder test fixture

`high_bframes_aac.mp4` is a two-second, 160x96 H.264 High Profile stream with
two B pictures and mono AAC-LC at 16 kHz. It exercises separate DTS/PTS,
composition offsets, video reordering, and audio/video presentation slicing.
