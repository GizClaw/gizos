#ifndef H2_LINUX_ALSA_ABI_H
#define H2_LINUX_ALSA_ABI_H

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

enum {
    H2_SND_PCM_STREAM_PLAYBACK = 0,
    H2_SND_PCM_NONBLOCK = 0x00000001,
    H2_SND_PCM_ACCESS_RW_INTERLEAVED = 3,
    H2_SND_PCM_FORMAT_S16_LE = 2,
};

typedef struct h2_linux_alsa_symbols {
    int (*pcm_open)(
        snd_pcm_t **pcm,
        const char *name,
        int stream,
        int mode);
    int (*pcm_set_params)(
        snd_pcm_t *pcm,
        int format,
        int access,
        unsigned int channels,
        unsigned int rate,
        int soft_resample,
        unsigned int latency);
    snd_pcm_sframes_t (*pcm_writei)(
        snd_pcm_t *pcm,
        const void *buffer,
        snd_pcm_uframes_t size);
    int (*pcm_wait)(snd_pcm_t *pcm, int timeout_ms);
    int (*pcm_recover)(snd_pcm_t *pcm, int error, int silent);
    int (*pcm_drop)(snd_pcm_t *pcm);
    int (*pcm_close)(snd_pcm_t *pcm);
} h2_linux_alsa_symbols_t;

#if defined(H2_LINUX_ALSA_TESTING)
int h2_linux_alsa_test_load_symbols(h2_linux_alsa_symbols_t *out_symbols);
#endif

#endif
