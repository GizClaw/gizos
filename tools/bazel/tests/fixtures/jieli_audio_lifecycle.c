#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2_jieli_wl82_atomic.h"

#define CONFIG_AUDIO_ADC_GAIN 80
#define OS_NO_ERR 0
#define AUDIO_REQ_ENC 1
#define AUDIO_REQ_DEC 2
#define AUDIO_ENC_OPEN 1
#define AUDIO_ENC_CLOSE 2
#define AUDIO_DEC_OPEN 3
#define AUDIO_DEC_START 4
#define AUDIO_DEC_STOP 5
#define AUDIO_DEC_SET_VOLUME 6
#define AUDIO_ATTR_REAL_TIME 1

static uint32_t timer_get_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}
static void h2_jieli_sdk_sleep_ms(uint32_t ms) {
    struct timespec delay = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    nanosleep(&delay, NULL);
}
struct h2_pal_mutex { pthread_mutex_t native; };
struct h2_pal_cond { pthread_cond_t native; };
static h2_pal_result_t fake_mutex_create(void *u, const h2_pal_mutex_config_t *c, h2_pal_mutex_t **out) {
    (void)u;
    (void)c;
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    assert(pthread_mutex_init(&(*out)->native, NULL) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t fake_mutex_destroy(void *u, h2_pal_mutex_t *m) {
    (void)u;
    assert(pthread_mutex_destroy(&m->native) == 0);
    free(m);
    return H2_PAL_OK;
}
static h2_pal_result_t fake_lock(void *u, h2_pal_mutex_t *m) {
    (void)u;
    assert(pthread_mutex_lock(&m->native) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t fake_try_lock(void *u, h2_pal_mutex_t *m) {
    (void)u;
    return pthread_mutex_trylock(&m->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t fake_unlock(void *u, h2_pal_mutex_t *m) {
    (void)u;
    assert(pthread_mutex_unlock(&m->native) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t fake_cond_create(void *u, const h2_pal_cond_config_t *c, h2_pal_cond_t **out) {
    (void)u;
    (void)c;
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    assert(pthread_cond_init(&(*out)->native, NULL) == 0);
    return H2_PAL_OK;
}
static struct timespec expires(uint32_t ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000u;
    t.tv_nsec += (long)(ms % 1000u) * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    return t;
}
static volatile uint32_t waiting;
static atomic_uint wait_entries, release_callback, stop_called;
static int hold_first_wake;
static h2_pal_result_t fake_wait(void *u, h2_pal_cond_t *c, h2_pal_mutex_t *m, uint32_t ms) {
    (void)u;
    h2_jieli_atomic_store_u32(&waiting, 1u);
    unsigned entry = atomic_fetch_add(&wait_entries, 1u);
    int rc;
    if (ms == UINT32_MAX) {
        rc = pthread_cond_wait(&c->native, &m->native);
    } else {
        struct timespec deadline = expires(ms);
        rc = pthread_cond_timedwait(&c->native, &m->native, &deadline);
    }
    if (hold_first_wake && entry == 0u) {
        assert(pthread_mutex_unlock(&m->native) == 0);
        while (atomic_load(&release_callback) == 0u) h2_jieli_sdk_sleep_ms(1);
        assert(pthread_mutex_lock(&m->native) == 0);
    }
    assert(rc == 0 || rc == ETIMEDOUT);
    return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t fake_broadcast(void *u, h2_pal_cond_t *c) {
    (void)u;
    assert(pthread_cond_broadcast(&c->native) == 0);
    return H2_PAL_OK;
}
static const h2_pal_sync_api_t *h2_jieli_wl82_platform_sync_api(void) {
    static const h2_pal_sync_vtable_t vt = {
        .create_mutex = fake_mutex_create, .destroy_mutex = fake_mutex_destroy,
        .lock_mutex = fake_lock, .try_lock_mutex = fake_try_lock, .unlock_mutex = fake_unlock,
        .create_cond = fake_cond_create, .wait_cond = fake_wait, .broadcast_cond = fake_broadcast,
    };
    static const h2_pal_sync_api_t api = {.vtable = &vt};
    return &api;
}
static h2_pal_result_t h2_jieli_wl82_cond_wait_owned(h2_pal_cond_t *c, h2_pal_mutex_t *m, uint32_t ms, int *locked) {
    *locked = 1;
    return fake_wait(NULL, c, m, ms);
}
/* SDK compatibility fake for the pre-fix source. The full-buffer write traps
 * instead of hanging the test process: that API has no deadline parameter. */
typedef struct { uint8_t *data; uint32_t capacity, count; } cbuffer_t;
static pthread_mutex_t sdk_cbuf_gate = PTHREAD_MUTEX_INITIALIZER;
typedef struct { unsigned count; } OS_SEM;
static void cbuf_init(cbuffer_t *b, void *data, uint32_t cap) { *b = (cbuffer_t){data, cap, 0}; }
static int cbuf_is_write_able(cbuffer_t *b, uint32_t n) { return n <= b->capacity - b->count; }
static void cbuf_clear(cbuffer_t *b) {
    assert(pthread_mutex_lock(&sdk_cbuf_gate) == 0);
    b->count = 0;
    assert(pthread_mutex_unlock(&sdk_cbuf_gate) == 0);
}
static uint32_t cbuf_write(cbuffer_t *b, const void *p, uint32_t n) {
    assert(pthread_mutex_lock(&sdk_cbuf_gate) == 0);
    if (n > b->capacity - b->count) {
        assert(pthread_mutex_unlock(&sdk_cbuf_gate) == 0);
        return 0;
    }
    memcpy(b->data + b->count, p, n);
    b->count += n;
    assert(pthread_mutex_unlock(&sdk_cbuf_gate) == 0);
    return n;
}
static uint32_t cbuf_get_data_size(cbuffer_t *b) { return b->count; }
static uint32_t cbuf_read(cbuffer_t *b, void *p, uint32_t n) {
    /* Pinned cbuf_read snapshots data_len before its lock; clear can invalidate
     * that snapshot. Queries similarly read data_len without that lock. */
    if (n > b->count) n = b->count;
    assert(pthread_mutex_lock(&sdk_cbuf_gate) == 0);
    assert(n <= b->count);
    memcpy(p, b->data, n);
    memmove(b->data, b->data + n, b->count - n);
    b->count -= n;
    assert(pthread_mutex_unlock(&sdk_cbuf_gate) == 0);
    return n;
}
static int os_sem_create(OS_SEM *s, unsigned n) { s->count = n; return 0; }
static void os_sem_set(OS_SEM *s, unsigned n) { s->count = n; }
static void os_sem_post(OS_SEM *s) { ++s->count; }
static int os_sem_pend(OS_SEM *s, uint32_t ticks) {
    if (s->count != 0u) { --s->count; return 0; }
    h2_jieli_sdk_sleep_ms(ticks * 10u);
    return -1;
}
struct audio_vfs_ops {
    int (*fread)(void *, void *, uint32_t);
    int (*fwrite)(void *, void *, uint32_t);
    int (*fclose)(void *);
    int (*flen)(void *);
};
struct audio_request {
    int cmd, channel, channel_bit_map, volume;
    uint32_t output_buf_len, sample_rate, frame_size, attr;
    const char *format, *sample_source, *dec_type;
    const struct audio_vfs_ops *vfs_ops;
    FILE *file;
};
union audio_req { struct audio_request enc, dec; };
struct server { struct audio_request request; };
static struct server *encoder, *decoder;
static int fail_command, live_servers;
static struct server *server_open(const char *name, const char *type) {
    (void)name;
    struct server *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    ++live_servers;
    if (strcmp(type, "enc") == 0) encoder = s;
    else decoder = s;
    return s;
}
static int server_request(struct server *s, int type, union audio_req *r) {
    (void)type;
    if (r->dec.cmd == AUDIO_DEC_STOP) atomic_store(&stop_called, 1u);
    if (r->dec.cmd == fail_command) return -1;
    if (r->dec.cmd == AUDIO_DEC_OPEN || r->enc.cmd == AUDIO_ENC_OPEN) s->request = r->dec;
    if (r->dec.cmd == AUDIO_DEC_START && s->request.vfs_ops != NULL) {
        int16_t scratch[8];
        /* Synchronous startup re-entry must not wait for a future writer. */
        assert(s->request.vfs_ops->fread(s->request.file, scratch, sizeof(scratch)) == -2);
    }
    if (r->dec.cmd == AUDIO_DEC_STOP && s->request.vfs_ops != NULL) {
        int16_t scratch[8];
        assert(s->request.vfs_ops->fread(s->request.file, scratch, sizeof(scratch)) == 0);
    }
    return 0;
}
static void server_close(struct server *s) { --live_servers; free(s); }
static cbuffer_t legacy_pcm;
static int close_case;
static volatile uint32_t legacy_writer;
static void *audio_pcm_play_open_no_sync(int rate, uint32_t frame, uint32_t drop, unsigned channel, unsigned volume, unsigned block) {
    (void)rate; (void)drop; (void)channel; (void)volume; (void)block;
    cbuf_init(&legacy_pcm, malloc(frame * 4u), frame * 4u);
    return &legacy_pcm;
}
static int audio_pcm_play_start(void *p) { (void)p; return 0; }
static int audio_pcm_play_set_block(void *p, unsigned b) { (void)p; (void)b; return 0; }
static int audio_pcm_play_set_volume(void *p, unsigned v) { (void)p; (void)v; return 0; }
static int audio_pcm_play_data_write(void *p, void *data, uint32_t n) {
    cbuffer_t *b = p;
    if (!cbuf_is_write_able(b, n) && close_case) {
        h2_jieli_atomic_store_u32(&legacy_writer, 1u);
        h2_jieli_atomic_store_u32(&waiting, 1u);
        for (;;) h2_jieli_sdk_sleep_ms(1);
    }
    assert(cbuf_is_write_able(b, n) && "legacy PCM would block forever despite caller deadline");
    return (int)cbuf_write(b, data, n);
}
static int audio_pcm_play_stop(void *p) {
    cbuffer_t *b = p;
    assert(h2_jieli_atomic_load_u32(&legacy_writer) == 0u && "stop frees a handle with an in-flight writer");
    free(b->data);
    return 0;
}
static int h2_jieli_ac791n_devkit_console_write(const void *p, size_t n, uint32_t ms) {
    (void)p; (void)n; (void)ms; return 0;
}
const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void);
typedef struct h2_jieli_ac791n_devkit_audio_idle {
  uint32_t open_tracks;         /* tracks not in the free state */
  uint32_t retained_operations; /* referenced writers, drainers, volume requests and callbacks */
  uint32_t ring_bytes;          /* PCM ring storage still allocated by tracks */
  uint32_t sdk_servers;         /* live encoder and decoder handles */
  uint32_t mic_open;            /* microphone session is not free */
  uint32_t speaker_started;
  uint64_t consumed_bytes;      /* PCM consumed from currently open tracks */
} h2_jieli_ac791n_devkit_audio_idle_t;

static atomic_uint allocations, frees, live_blocks;
static void *counted_malloc(size_t size) {
    void *block = malloc(size);
    if (block != NULL) { ++allocations; ++live_blocks; }
    return block;
}
static void counted_free(void *block) {
    if (block != NULL) { ++frees; assert(atomic_fetch_sub(&live_blocks, 1u) > 0u); }
    free(block);
}
#define malloc counted_malloc
#define free counted_free
/* REAL_PROVIDER */
#undef malloc
#undef free

static h2_audio_frame_t frame_for(int16_t *data) {
    return (h2_audio_frame_t){.data = data, .capacity = 640, .bytes = 640,
        .sample_rate_hz = 16000, .channels = 1, .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .samples_per_channel = 320};
}
static h2_pal_audio_track_t *make_track_result(size_t frames, int expected) {
    assert(audio_start_speaker(NULL) == H2_AUDIO_OK);
    h2_audio_track_config_t config = {.buffer_frames = frames, .volume_factor_milli = 1000,
        .format = {.sample_rate_hz = 16000, .channels = 1, .sample_format = H2_AUDIO_SAMPLE_S16LE}};
    h2_pal_audio_track_t *track = NULL;
    assert(audio_create_track(NULL, &config, &track) == expected);
    if (expected != H2_AUDIO_OK) assert(track == NULL);
    return track;
}
static h2_pal_audio_track_t *make_track(size_t frames) { return make_track_result(frames, H2_AUDIO_OK); }
static void consume(void) {
    int16_t scratch[640];
    if (decoder != NULL) {
        assert(decoder->request.vfs_ops->fread(decoder->request.file, scratch, sizeof(scratch)) == 1280);
    } else {
        assert(cbuf_read(&legacy_pcm, scratch, 640) == 640);
    }
}
struct write_job { h2_pal_audio_track_t *track; int result; };
static void *close_worker(void *arg) {
    struct write_job *job = arg;
    job->result = track_close(job->track);
    return NULL;
}
static void *decoder_worker(void *arg) {
    (void)arg;
    struct audio_request request = decoder->request;
    int16_t scratch[640];
    assert(request.vfs_ops->fread(request.file, scratch, sizeof(scratch)) == 0);
    return NULL;
}
static void *drain_worker(void *arg) {
    struct write_job *job = arg;
    job->result = track_drain(job->track, 1000u);
    return NULL;
}
static void *write_worker(void *arg) {
    struct write_job *job = arg;
    int16_t data[320] = {0};
    h2_audio_frame_t frame = frame_for(data);
    job->result = track_write(job->track, &frame, UINT32_MAX);
    return NULL;
}
static void *mic_producer(void *unused) {
    (void)unused;
    int16_t data[320];
    for (int iteration = 1; iteration <= 2000; ++iteration) {
        for (unsigned i = 0; i < 320; ++i) data[i] = (int16_t)iteration;
        assert(encoder->request.vfs_ops->fwrite(encoder->request.file, data, sizeof(data)) == (int)sizeof(data));
    }
    return NULL;
}
static void assert_idle(void) {
    h2_jieli_ac791n_devkit_audio_idle_t idle;
    memset(&idle, 0xff, sizeof(idle));
    assert(h2_jieli_ac791n_devkit_audio_idle_probe(NULL) == H2_AUDIO_ERR_INVALID_ARG);
    assert(h2_jieli_ac791n_devkit_audio_idle_probe(&idle) == H2_AUDIO_OK);
    assert(idle.open_tracks == 0u && idle.retained_operations == 0u && idle.ring_bytes == 0u);
    assert(idle.sdk_servers == 0u && idle.mic_open == 0u && idle.speaker_started == 0u);
    assert(idle.consumed_bytes == 0u && live_servers == 0);
    assert(live_blocks == 0u && allocations == frees);
}
static void normal_stop(h2_pal_audio_track_t *music, h2_pal_audio_track_t *mic) {
    assert(audio_stop_mic(NULL) == H2_AUDIO_OK);
    assert(audio_stop_speaker(NULL) == H2_AUDIO_OK);
    if (music != NULL) assert(track_close(music) == H2_AUDIO_OK);
    if (mic != NULL) assert(track_close(mic) == H2_AUDIO_OK);
    assert_idle();
}
static void test_cycles(void) {
    uintptr_t generation = 0u;
    int16_t data[320] = {1}, scratch[640];
    h2_audio_frame_t frame = frame_for(data);
    for (unsigned cycle = 0u; cycle < 50u; ++cycle) {
        assert(audio_start_mic(NULL) == H2_AUDIO_OK);
        assert((uintptr_t)encoder->request.file > generation);
        generation = (uintptr_t)encoder->request.file;
        h2_pal_audio_track_t *music = make_track(1);
        struct audio_request music_request = decoder->request;
        assert((uintptr_t)music_request.file > generation);
        generation = (uintptr_t)music_request.file;
        h2_audio_track_config_t config = {.name = "audio-system-mic", .buffer_frames = 1u,
            .volume_factor_milli = 1000u,
            .format = {.sample_rate_hz = 16000u, .channels = 1u, .sample_format = H2_AUDIO_SAMPLE_S16LE}};
        h2_pal_audio_track_t *mic = NULL;
        assert(audio_create_track(NULL, &config, &mic) == H2_AUDIO_OK);
        assert((uintptr_t)decoder->request.file > generation);
        generation = (uintptr_t)decoder->request.file;
        assert(encoder->request.vfs_ops->fwrite(encoder->request.file, data, sizeof(data)) == 640);
        assert(audio_mic_read(NULL, &frame, 0u) == H2_AUDIO_OK && frame.bytes == 640u);
        assert(track_write(music, &frame, 0u) == H2_AUDIO_OK);
        assert(track_write(mic, &frame, 0u) == H2_AUDIO_OK);
        h2_jieli_ac791n_devkit_audio_idle_t probe;
        assert(h2_jieli_ac791n_devkit_audio_idle_probe(&probe) == H2_AUDIO_OK);
        assert(probe.open_tracks == 2u && probe.ring_bytes == 1280u && probe.sdk_servers == 3u);
        assert(probe.mic_open == 1u && probe.speaker_started == 1u && probe.retained_operations == 0u);
        assert(music_request.vfs_ops->fread(music_request.file, scratch, sizeof(scratch)) == 1280);
        consume();
        assert(h2_jieli_ac791n_devkit_audio_idle_probe(&probe) == H2_AUDIO_OK);
        assert(probe.consumed_bytes == 1280u && probe.ring_bytes == 1280u);
        normal_stop(music, mic);
    }
    assert(allocations == 100u);
}
static void test_cycle_blocked_write(void) {
    h2_pal_audio_track_t *track = make_track(1);
    int16_t data[320] = {1};
    h2_audio_frame_t frame = frame_for(data);
    assert(track_write(track, &frame, 0u) == H2_AUDIO_OK);
    struct write_job job = {.track = track};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, write_worker, &job) == 0);
    while (h2_jieli_atomic_load_u32(&waiting) == 0u) h2_jieli_sdk_sleep_ms(1u);
    h2_jieli_ac791n_devkit_audio_idle_t probe;
    assert(h2_jieli_ac791n_devkit_audio_idle_probe(&probe) == H2_AUDIO_OK);
    assert(probe.retained_operations == 1u);
    normal_stop(track, NULL);
    assert(pthread_join(worker, NULL) == 0);
    assert(job.result == H2_AUDIO_ERR_INVALID_STATE);
    track = make_track(1);
    assert(track_write(track, &frame, 0u) == H2_AUDIO_OK);
    consume();
    normal_stop(track, NULL);
}
static void test_cycle_stale_callback(void) {
    int16_t data[320] = {1}, scratch[640];
    h2_audio_frame_t frame = frame_for(data);
    assert(audio_start_mic(NULL) == H2_AUDIO_OK);
    h2_pal_audio_track_t *track = make_track(1);
    struct audio_request old_encoder = encoder->request, old_decoder = decoder->request;
    normal_stop(track, NULL);
    assert(audio_start_mic(NULL) == H2_AUDIO_OK);
    track = make_track(1);
    assert(old_encoder.vfs_ops->fwrite(old_encoder.file, data, sizeof(data)) == 640);
    assert(audio_mic_read(NULL, &frame, 0u) == H2_AUDIO_ERR_WOULD_BLOCK && frame.bytes == 0u);
    assert(encoder->request.vfs_ops->fwrite(encoder->request.file, data, sizeof(data)) == 640);
    assert(audio_mic_read(NULL, &frame, 0u) == H2_AUDIO_OK && frame.bytes == 640u);
    assert(track_write(track, &frame, 0u) == H2_AUDIO_OK);
    assert(old_decoder.vfs_ops->fread(old_decoder.file, scratch, sizeof(scratch)) == 0);
    consume();
    normal_stop(track, NULL);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "cycles") == 0) { test_cycles(); return 0; }
    if (strcmp(argv[1], "cycle_blocked_write") == 0) { test_cycle_blocked_write(); return 0; }
    if (strcmp(argv[1], "cycle_stale_callback") == 0) { test_cycle_stale_callback(); return 0; }
    close_case = strcmp(argv[1], "close") == 0;
    /* Keep both pre/post SDK surfaces warning-free without suppressing warnings. */
    (void)h2_jieli_wl82_platform_sync_api;
    (void)h2_jieli_wl82_cond_wait_owned;
    (void)cbuf_clear; (void)cbuf_get_data_size;
    (void)os_sem_create; (void)os_sem_set; (void)os_sem_post; (void)os_sem_pend;
    (void)audio_pcm_play_open_no_sync; (void)audio_pcm_play_start;
    (void)audio_pcm_play_set_block; (void)audio_pcm_play_set_volume;
    (void)audio_pcm_play_data_write; (void)audio_pcm_play_stop;
    int16_t data[320] = {1};
    h2_audio_frame_t frame = frame_for(data);
    if (strncmp(argv[1], "create_failure", 14) == 0) {
        fail_command = strcmp(argv[1], "create_failure_open") == 0 ? AUDIO_DEC_OPEN : AUDIO_DEC_START;
        assert(make_track_result(1, H2_AUDIO_ERR_IO) == NULL);
        assert(live_servers == 0);
        fail_command = 0;
        h2_pal_audio_track_t *track = make_track(1);
        assert(track_close(track) == H2_AUDIO_OK);
        assert(live_servers == 0);
        return 0;
    }
    if (strncmp(argv[1], "mic_", 4) == 0) {
        assert(audio_start_mic(NULL) == H2_AUDIO_OK);
        struct audio_request old = encoder->request;
        if (strcmp(argv[1], "mic_generation") == 0) {
            assert(audio_stop_mic(NULL) == H2_AUDIO_OK);
            assert(audio_start_mic(NULL) == H2_AUDIO_OK);
            assert(old.vfs_ops->fwrite(old.file, data, sizeof(data)) == (int)sizeof(data));
            assert(audio_mic_read(NULL, &frame, 0) == H2_AUDIO_ERR_WOULD_BLOCK);
        } else if (strcmp(argv[1], "mic_tokens") == 0) {
            assert(old.vfs_ops->fwrite(old.file, data, sizeof(data)) == (int)sizeof(data));
            assert(audio_mic_read(NULL, &frame, 0) == H2_AUDIO_OK);
            uint32_t start = timer_get_ms();
            assert(audio_mic_read(NULL, &frame, 40) == H2_AUDIO_ERR_WOULD_BLOCK);
            assert(timer_get_ms() - start >= 35u);
        } else {
            pthread_t producer;
            assert(pthread_create(&producer, NULL, mic_producer, NULL) == 0);
            for (int iteration = 0; iteration < 2000; ++iteration) {
                int rc = audio_mic_read(NULL, &frame, 0);
                assert(rc == H2_AUDIO_OK || rc == H2_AUDIO_ERR_WOULD_BLOCK);
                if (rc == H2_AUDIO_OK) {
                    for (unsigned i = 1; i < frame.bytes / 2u; ++i) assert(data[i] == data[0]);
                }
            }
            assert(pthread_join(producer, NULL) == 0);
        }
        assert(audio_stop_mic(NULL) == H2_AUDIO_OK);
    } else {
        h2_pal_audio_track_t *track = make_track(strcmp(argv[1], "drain_target") == 0 ? 2u : 1u);
        if (strcmp(argv[1], "close_decoder") == 0) {
            assert(decoder != NULL);
            hold_first_wake = 1;
            pthread_t reader, closer;
            struct write_job job = {.track = track};
            assert(pthread_create(&reader, NULL, decoder_worker, NULL) == 0);
            while (atomic_load(&wait_entries) == 0u) h2_jieli_sdk_sleep_ms(1);
            assert(pthread_create(&closer, NULL, close_worker, &job) == 0);
            while (atomic_load(&wait_entries) < 2u && atomic_load(&stop_called) == 0u) h2_jieli_sdk_sleep_ms(1);
            assert(atomic_load(&stop_called) == 0u);
            atomic_store(&release_callback, 1u);
            assert(pthread_join(reader, NULL) == 0);
            assert(pthread_join(closer, NULL) == 0);
            assert(job.result == H2_AUDIO_OK && atomic_load(&stop_called) == 1u);
            return 0;
        }
        assert(track_write(track, &frame, 0) == H2_AUDIO_OK);
        if (strcmp(argv[1], "write") == 0) {
            assert(track_write(track, &frame, 0) == H2_AUDIO_ERR_WOULD_BLOCK);
            uint32_t start = timer_get_ms();
            assert(track_write(track, &frame, 40) == H2_AUDIO_ERR_WOULD_BLOCK);
            assert(timer_get_ms() - start >= 35u && timer_get_ms() - start < 200u);
        } else if (strcmp(argv[1], "drain") == 0) {
            assert(track_drain(track, 0) == H2_AUDIO_ERR_WOULD_BLOCK);
            consume();
            assert(track_drain(track, 0) == H2_AUDIO_OK);
        } else if (strcmp(argv[1], "drain_target") == 0) {
            struct write_job job = {.track = track};
            pthread_t worker;
            assert(pthread_create(&worker, NULL, drain_worker, &job) == 0);
            uint32_t start = timer_get_ms();
            while (h2_jieli_atomic_load_u32(&waiting) == 0u && timer_get_ms() - start < 500u) h2_jieli_sdk_sleep_ms(1);
            assert(h2_jieli_atomic_load_u32(&waiting) != 0u);
            assert(track_write(track, &frame, 0) == H2_AUDIO_OK);
            consume();
            assert(pthread_join(worker, NULL) == 0);
            assert(job.result == H2_AUDIO_OK);
            assert(track_drain(track, 0) == H2_AUDIO_ERR_WOULD_BLOCK);
            consume();
            assert(track_drain(track, 0) == H2_AUDIO_OK);
        } else if (strcmp(argv[1], "close") == 0) {
            struct write_job job = {.track = track};
            pthread_t worker;
            assert(pthread_create(&worker, NULL, write_worker, &job) == 0);
            while (h2_jieli_atomic_load_u32(&waiting) == 0u) h2_jieli_sdk_sleep_ms(1);
            assert(track_close(track) == H2_AUDIO_OK);
            assert(pthread_join(worker, NULL) == 0);
            assert(job.result == H2_AUDIO_ERR_INVALID_STATE);
            return 0;
        } else {
            fail_command = AUDIO_DEC_STOP;
            assert(track_close(track) == H2_AUDIO_ERR_IO);
            return 0;
        }
        assert(track_close(track) == H2_AUDIO_OK);
    }
    return 0;
}
