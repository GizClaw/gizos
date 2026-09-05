"""Regression checks for the JieLi streaming PCM Audio PAL."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
AUDIO_SOURCE = (
    ROOT
    / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_audio.c"
)
LOADER_TASK_POLICY = (
    ROOT
    / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_task_policy.c"
)


class JieliAudioPalTest(unittest.TestCase):
    def test_playback_uses_jieli_pcm_play_api(self):
        source = AUDIO_SOURCE.read_text()
        self.assertIn("audio_pcm_play_open_no_sync(", source)
        self.assertIn("audio_pcm_play_start(", source)
        self.assertIn("audio_pcm_play_set_block(", source)
        self.assertIn("audio_pcm_play_data_write(", source)
        self.assertIn("audio_pcm_play_stop(", source)
        self.assertNotIn("static int track_vfs_read(", source)

    def test_display_uses_emi_completion_semaphore(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_input.c").read_text()
        self.assertRegex(
            source,
            r"EMI_SET_ISR_CB,\s*display_emi_send_complete",
        )
        self.assertIn("dev_ioctl(state->device, EMI_USE_SEND_SEM, 1)", source)
        self.assertIn("IOCTL_EMI_WRITE_NON_BLOCK, 1", source)
        self.assertGreaterEqual(
            source.count("IOCTL_EMI_WRITE_NON_BLOCK_FLUSH"), 2
        )
        draw = source[source.index("static int display_draw_bitmap"):
                      source.index("static int display_present")]
        self.assertEqual(draw.count("dev_write("), 1)
        self.assertLess(
            draw.index("IOCTL_EMI_WRITE_NON_BLOCK_FLUSH"),
            draw.index("dev_write("),
        )

    def test_mp4_ready_does_not_reinitialize_live_watchdog(self):
        source = (ROOT / "projects/example/targets/h2loader_tar_zlib/display/"
                  "jieli_ac791n_devkit/src/mp4_player_small_pal.c").read_text()
        ready = source[source.index("static h2_pal_result_t confirm_ready"):
                       source.index("static int mp4_watchdog_poll")]
        self.assertNotIn("wdt_init(", ready)

    def test_mono_pal_stream_uses_stereo_dac_like_official_pcm_helper(self):
        source = AUDIO_SOURCE.read_text()
        self.assertIn("audio_pcm_play_open_no_sync(", source)
        self.assertIn("0u, 1u,", source)

    def test_dac_uses_official_nonblocking_start_before_display(self):
        source = AUDIO_SOURCE.read_text()
        create_start = source.index("static int audio_create_track(")
        create_end = source.index("static int audio_stop_speaker", create_start)
        create_track = source[create_start:create_end]
        self.assertIn("audio_pcm_play_open_no_sync(", create_track)
        self.assertIn("audio_pcm_play_start(", create_track)
        self.assertIn("audio_pcm_play_set_block(", create_track)
        self.assertLess(
            create_track.index("audio_pcm_play_open_no_sync("),
            create_track.index("audio_pcm_play_start("),
        )
        self.assertLess(
            create_track.index("audio_pcm_play_start("),
            create_track.index("audio_pcm_play_set_block("),
        )

    def test_audio_stage_logs_bypass_sdk_printf(self):
        source = AUDIO_SOURCE.read_text()
        self.assertIn("h2_jieli_ac791n_devkit_console_write(", source)
        self.assertNotIn('printf("H2_JIELI_AUDIO', source)

    def test_pal_honors_low_latency_frame_queues(self):
        source = AUDIO_SOURCE.read_text()
        self.assertIn("H2_AUDIO_MIC_QUEUE_FRAMES = 4", source)
        self.assertIn("config->buffer_frames != 0u", source)
        self.assertIn("wanted_cache_bytes", source)
        self.assertIn("available > H2_AUDIO_FRAME_BYTES", source)
        self.assertNotIn("H2_AUDIO_TRACK_BUFFER_BYTES = 16384", source)

    def test_loader_policy_supports_audio_enabled_shared_layout(self):
        policy = LOADER_TASK_POLICY.read_text()
        for task in ("audio_server", "audio_decoder", "audio_encoder", "audio_mix"):
            self.assertIn(f'{{"{task}"', policy)


if __name__ == "__main__":
    unittest.main()
