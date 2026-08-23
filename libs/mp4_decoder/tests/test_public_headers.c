#include "h2_mp4_decoder.h"

#include <assert.h>

int main(void) {
    h2_mp4_decoder_config_t config = {0};
    h2_mp4_decoder_frame_info_t frame = {0};
    assert(config.max_samples == 0u);
    assert(frame.video_plane_count == 0u);
    return 0;
}
