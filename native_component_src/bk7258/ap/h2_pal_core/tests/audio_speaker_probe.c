#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_play.h"
#include <driver/audio_ring_buff.h>

enum { SPK_DATA_READ_IDLE, SPK_DATA_READ_START };
typedef struct {
  uint32_t frame_size;
  uint8_t* read_buff;
  RingBufferContext dma_rb;
  int dac_dma_id;
  void* spk_data_read_msg_que;
  void* sem;
  bool running;
  audio_play_sta_t state;
} onboard_speaker_play_priv_t;

static onboard_speaker_play_priv_t* active;
static uint32_t cursor, pause_address;
static bool dma_enabled, dac_enabled, writer_active, idle_pending;
static unsigned sem_count, unsafe_writes, primed;
static int fail_idle, fail_start, fail_ack, fail_dma, fail_source;
static void deliver_idle(void);

static uint32_t address(const void* p) { return (uint32_t)(uintptr_t)p; }
uint32_t bk_dma_get_enable_status(dma_id_t id) {
  (void)id;
  return dma_enabled;
}
uint32_t dma_get_src_read_addr(dma_id_t id) {
  (void)id;
  return cursor;
}
bk_err_t dma_set_src_pause_addr(dma_id_t id, uint32_t value) {
  (void)id;
  if (writer_active) ++unsafe_writes;
  pause_address = value;
  return BK_OK;
}
bk_err_t dma_set_dst_pause_addr(dma_id_t id, uint32_t value) {
  (void)id;
  (void)value;
  return BK_OK;
}
bk_err_t bk_dma_set_src_start_addr(dma_id_t id, uint32_t value) {
  (void)id;
  if (fail_source) return BK_FAIL;
  assert(!dma_enabled);
  if (writer_active) ++unsafe_writes;
  cursor = value;
  return BK_OK;
}
bk_err_t bk_dma_set_transfer_len(dma_id_t id, uint32_t bytes) {
  (void)id;
  assert(bytes == active->frame_size);
  return BK_OK;
}
static bk_err_t bk_dma_start(dma_id_t id) {
  (void)id;
  if (fail_dma) return BK_FAIL;
  dma_enabled = true;
  return BK_OK;
}
static bk_err_t bk_dma_stop(dma_id_t id) {
  (void)id;
  dma_enabled = false; /* Disable retains a paused cursor until reprogrammed. */
  return BK_OK;
}
static bk_err_t bk_aud_dac_start(void) {
  dac_enabled = true;
  return BK_OK;
}
static bk_err_t bk_aud_dac_stop(void) {
  dac_enabled = false;
  return BK_OK;
}
bk_err_t rtos_set_semaphore(void** sem) {
  (void)sem;
  ++sem_count;
  return BK_OK;
}
bk_err_t rtos_get_semaphore(void** sem, uint32_t timeout) {
  (void)sem;
  assert(timeout == BEKEN_WAIT_FOREVER);
  if (fail_ack) return BK_FAIL;
  if (idle_pending) deliver_idle();
  if (!sem_count) return BK_FAIL;
  --sem_count;
  return BK_OK;
}
static bk_err_t spk_data_read_send_msg(void* queue, int operation, void* arg) {
  (void)queue;
  (void)arg;
  if (operation == SPK_DATA_READ_IDLE) {
    if (fail_idle) return BK_FAIL;
    idle_pending = true; /* Deliberately leave an old ring write in flight. */
  } else {
    if (fail_start) return BK_FAIL;
    const uint32_t offset = cursor - address(active->dma_rb.address);
    primed = (active->dma_rb.wp + active->dma_rb.capacity - offset) %
             active->dma_rb.capacity;
    writer_active = active->running = true;
  }
  return BK_OK;
}
static void deliver_idle(void) {
  onboard_speaker_play_priv_t* spk_data_read_handle = active;
  uint32_t wait_time = 0;
#include "sdk_speaker_idle.inc"
  (void)wait_time;
  idle_pending = false;
  writer_active = active->running;
}
static audio_play_sta_t onboard_speaker_state_get(
    onboard_speaker_play_priv_t* p) {
  return p->state;
}
static void onboard_speaker_state_set(onboard_speaker_play_priv_t* p,
                                      audio_play_sta_t state) {
  p->state = state;
}
static int onboard_speaker_mute(void) { return BK_OK; }
static int onboard_speaker_unmute(void) { return BK_OK; }
static int onboard_speaker_set_volume(int volume) {
  (void)volume;
  return BK_OK;
}
#define ONBOARD_SPK_PLAY_CHECK_NULL(p) \
  do {                                 \
    if (!(p)) return BK_FAIL;          \
  } while (0)
#define RWP_SAFE_INTERVAL 0
#define os_memset memset
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#include "sdk_audio_ring.inc"
#include "sdk_speaker_functions.inc"

static void reset(onboard_speaker_play_priv_t* p, uint8_t* buffer,
                  uint8_t* scratch, uint32_t offset) {
  *p = (onboard_speaker_play_priv_t){.frame_size = 32u,
                                     .read_buff = scratch,
                                     .dma_rb = {.address = buffer,
                                                .capacity = 72u,
                                                .wp = offset,
                                                .rp = offset,
                                                .dma_id = 0,
                                                .dma_type = RB_DMA_TYPE_READ},
                                     .running = true,
                                     .state = AUDIO_PLAY_STA_RUNNING};
  active = p;
  cursor = pause_address = address(buffer) + offset;
  dma_enabled = dac_enabled = writer_active = true;
  idle_pending = false;
  sem_count = unsafe_writes = primed = 0;
  fail_idle = fail_start = fail_ack = fail_dma = fail_source = 0;
}
int main(int argc, char** argv) {
  assert(argc == 2);
  onboard_speaker_play_priv_t player;
  uint8_t buffer[72], scratch[32];
  audio_play_t api = {.play_ctx = &player};
  for (uint32_t offset = 0; offset < sizeof(buffer); offset += 4u) {
    reset(&player, buffer, scratch, offset);
    assert(onboard_speaker_play_control(&api, AUDIO_PLAY_PAUSE) == BK_OK);
    if (strcmp(argv[1], "bootstrap") == 0 && idle_pending) deliver_idle();
    assert(onboard_speaker_play_control(&api, AUDIO_PLAY_RESUME) == BK_OK);
    if (strcmp(argv[1], "handoff") == 0 && unsafe_writes) {
      puts("unquiesced ring writer");
      return 19;
    }
    if (strcmp(argv[1], "bootstrap") == 0 &&
        (primed != 2u * player.frame_size || cursor == pause_address)) {
      printf("unprimed paused DMA offset=%u primed=%u\n", offset, primed);
      return 20;
    }
    if (strcmp(argv[1], "handoff") == 0 || strcmp(argv[1], "bootstrap") == 0) {
      assert(!unsafe_writes && primed == 64u && dma_enabled && dac_enabled);
      assert(player.state == AUDIO_PLAY_STA_RUNNING && writer_active);
      assert(onboard_speaker_play_control(&api, AUDIO_PLAY_RESUME) == BK_OK);
      assert(!unsafe_writes && primed == 64u);
      continue;
    }
  }
  if (strcmp(argv[1], "errors") == 0) {
    reset(&player, buffer, scratch, 0);
    fail_idle = 1;
    assert(onboard_speaker_play_control(&api, AUDIO_PLAY_PAUSE) == BK_FAIL);
    assert(player.state == AUDIO_PLAY_STA_RUNNING && dma_enabled);
    reset(&player, buffer, scratch, 0);
    fail_ack = 1;
    assert(onboard_speaker_play_control(&api, AUDIO_PLAY_PAUSE) == BK_FAIL);
    assert(player.state == AUDIO_PLAY_STA_RUNNING && dma_enabled);
    for (unsigned failure = 0; failure < 4; ++failure) {
      reset(&player, buffer, scratch, 0);
      assert(onboard_speaker_play_control(&api, AUDIO_PLAY_PAUSE) == BK_OK);
      if (failure == 0) fail_source = 1;
      if (failure == 1) fail_start = 1;
      if (failure == 2) fail_dma = 1;
      if (failure == 3) player.dma_rb.capacity = 40u;
      assert(onboard_speaker_play_control(&api, AUDIO_PLAY_RESUME) == BK_FAIL);
      assert(player.state == AUDIO_PLAY_STA_PAUSED && !dma_enabled &&
             !dac_enabled);
    }
  }
  puts("PASS paused offsets, in-flight writer handoff and error propagation");
  return 0;
}
