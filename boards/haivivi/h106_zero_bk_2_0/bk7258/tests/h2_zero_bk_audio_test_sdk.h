#ifndef H2_ZERO_BK_AUDIO_TEST_SDK_H
#define H2_ZERO_BK_AUDIO_TEST_SDK_H
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_audio_mixer.h"

typedef struct {
  uint32_t sample_rate;
  uint16_t frame_samples_per_channel;
  uint8_t channels, mic_channels, speaker_channels, bits_per_sample,
      default_volume, default_mic_gain, mic_gain_min, mic_gain_max, frame_count;
} h2_bk7258_audio_config_t;
extern h2_bk7258_audio_config_t h2_bk7258_audio_config;
const h2_pal_mem_api_t* h2_bk7258_board_psram_allocator(void);
const h2_pal_mem_api_t* h2_bk7258_board_default_allocator(void);
const h2_pal_queue_api_t* h2_bk7258_board_queue_api(void);
const h2_pal_sync_api_t* h2_bk7258_board_sync_api(void);
const h2_pal_task_api_t* h2_bk7258_board_task_api(void);
h2_pal_audio_t* h2_bk7258_board_audio(void);
int h2_bk7258_board_audio_power_set(int enabled);
#define BK_OK 0
#define kNoErr 0
#define BK_LOGE(...) ((void)0)
#define os_memset memset
#define os_memcpy memcpy
void* test_malloc(size_t size);
void test_free(void* ptr);
#define os_malloc test_malloc
#define os_free test_free
typedef int beken_mutex_t;
int rtos_init_mutex(beken_mutex_t* mutex);
int rtos_lock_mutex(beken_mutex_t* mutex);
int rtos_unlock_mutex(beken_mutex_t* mutex);
void rtos_delay_milliseconds(unsigned ms);
unsigned rtos_get_free_heap_size(void);
unsigned rtos_get_psram_free_heap_size(void);
typedef struct {
  int unused;
} AECContext;
uint32_t aec_size(uint32_t size);
void aec_init(AECContext* ctx, int16_t rate);
void aec_ctrl(AECContext* ctx, int command, uintptr_t value);
void aec_proc(AECContext* ctx, int16_t* ref, int16_t* mic, int16_t* out);
int bk_audio_osi_funcs_init(void);
enum {
  AEC_CTRL_CMD_GET_FRAME_SAMPLE,
  AEC_CTRL_CMD_GET_RX_BUF,
  AEC_CTRL_CMD_GET_TX_BUF,
  AEC_CTRL_CMD_GET_OUT_BUF,
  AEC_CTRL_CMD_SET_FLAGS,
  AEC_CTRL_CMD_SET_MIC_DELAY,
  AEC_CTRL_CMD_SET_EC_DEPTH,
  AEC_CTRL_CMD_SET_TxRxThr,
  AEC_CTRL_CMD_SET_TxRxFlr,
  AEC_CTRL_CMD_SET_REF_SCALE,
  AEC_CTRL_CMD_SET_VOL,
  AEC_CTRL_CMD_SET_NS_LEVEL,
  AEC_CTRL_CMD_SET_NS_PARA,
  AEC_CTRL_CMD_SET_DRC
};
typedef struct {
  int unused;
} audio_play_t;
typedef struct {
  int unused;
} audio_record_t;
typedef struct {
  int nChans, sampRate, bitsPerSample, volume, frame_size, pool_size, play_mode;
} audio_play_cfg_t;
typedef struct {
  int nChans, sampRate, bitsPerSample, adc_gain, mic_mode, frame_size,
      pool_size;
} audio_record_cfg_t;
#define DEFAULT_AUDIO_PLAY_CONFIG() ((audio_play_cfg_t){0})
#define DEFAULT_AUDIO_RECORD_CONFIG() ((audio_record_cfg_t){0})
enum {
  AUDIO_PLAY_MODE_DIFFEN,
  AUDIO_PLAY_ONBOARD_SPEAKER,
  AUDIO_MIC_MODE_DIFFEN,
  AUDIO_RECORD_ONBOARD_MIC,
  AUDIO_RECORD_RESUME,
  AUDIO_RECORD_PAUSE,
  AUDIO_PLAY_RESUME,
  AUDIO_PLAY_UNMUTE,
  AUDIO_PLAY_MUTE,
  AUDIO_PLAY_PAUSE
};
audio_play_t* audio_play_create(int type, audio_play_cfg_t* cfg);
audio_record_t* audio_record_create(int type, audio_record_cfg_t* cfg);
int audio_play_write_data(audio_play_t* play, char* data, uint32_t size);
int audio_record_read_data(audio_record_t* record, char* data, uint32_t size);
int audio_record_open(audio_record_t* record);
int audio_record_close(audio_record_t* record);
int audio_record_destroy(audio_record_t* record);
int audio_record_control(audio_record_t* record, int cmd);
int audio_play_set_adc_gain(audio_record_t* record, int gain);
int audio_play_open(audio_play_t* play);
int audio_play_close(audio_play_t* play);
int audio_play_control(audio_play_t* play, int cmd);
int audio_play_set_volume(audio_play_t* play, int volume);
#endif
