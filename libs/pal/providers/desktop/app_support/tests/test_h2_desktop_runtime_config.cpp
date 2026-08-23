#include "h2_desktop_app_support.h"

#include "h2_ffmpeg.h"

#include <assert.h>

int main(void) {
    const h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);

    assert(config.audio_decoder == h2_ffmpeg_audio_decoder_api());
    assert(config.video_decoder == h2_ffmpeg_video_decoder_api());
    return 0;
}
