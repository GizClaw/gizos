#include "h2_ffmpeg.h"

#include <assert.h>
#include <stdlib.h>

static void *allocate(void *user, size_t size) { (void)user; return malloc(size); }
static void release(void *user, void *ptr) { (void)user; free(ptr); }
static const h2_pal_mem_vtable_t s_vtable = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t s_allocator = {.vtable = &s_vtable};

int main(void) {
    const h2_audio_decoder_config_t config = {
        .pcm_allocator = &s_allocator,
        .preferred_format = H2_AUDIO_SAMPLE_S16LE,
    };
    h2_pal_audio_decoder_session_t *session = NULL;
    const h2_pal_audio_decoder_api_t *api =
        h2_ffmpeg_audio_decoder_api();
    assert(h2_pal_audio_decoder_open(api, &config, &session) == H2_PAL_OK);
    assert(session != NULL);
    assert(h2_pal_audio_decoder_close(api, session) == H2_PAL_OK);
    return 0;
}
