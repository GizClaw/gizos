/* Run the pinned ADC setup body; hardware calls only capture its descriptor. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int bk_err_t;
typedef int dma_id_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define CONFIG_SPE 1
#define DMA_ID_0 0
#define DMA_ID_MAX 16
#define DMA_DEV_AUDIO 3
#define DMA_DEV_AUDIO_RX 4
#define DMA_DEV_DTCM 0
#define DMA_WORK_MODE_REPEAT 1
#define DMA_DATA_WIDTH_16BITS 1
#define DMA_DATA_WIDTH_32BITS 2
#define DMA_ADDR_INC_ENABLE 1
#define DMA_ADDR_LOOP_ENABLE 1
#define DMA_ATTR_SEC 1
#define DMA_CARRY_MIC_RINGBUF_SAFE_INTERVAL 8
#define RB_DMA_TYPE_WRITE 1
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#define os_malloc malloc
#define os_memset memset
#define ONBOARD_MIC_RECORD_CHECK_NULL(p) \
  do {                                   \
    if (!(p)) return BK_FAIL;            \
  } while (0)

typedef struct {
  uint32_t dev, width, addr_inc_en, addr_loop_en, start_addr, end_addr;
} endpoint_t;
typedef struct {
  uint32_t mode, chan_prio;
  endpoint_t src, dst;
} dma_config_t;
typedef struct {
  uint8_t* address;
  unsigned bytes;
} RingBufferContext;
typedef struct {
  struct {
    unsigned nChans;
  } config;
  unsigned frame_size, frame_num;
  dma_id_t adc_dma_id;
  int32_t* dma_rb_addr;
  RingBufferContext dma_rb;
} onboard_mic_record_priv_t;

static dma_config_t captured;
static unsigned transfer_bytes, interrupts, security, initialized, released;
static int fifo_result, init_result;
static dma_id_t bk_dma_alloc(unsigned user) {
  assert(user == DMA_DEV_AUDIO);
  return 5;
}
static void ring_buffer_init(RingBufferContext* rb, uint8_t* address,
                             unsigned bytes, dma_id_t id, unsigned type) {
  assert(id == 5 && type == RB_DMA_TYPE_WRITE);
  *rb = (RingBufferContext){address, bytes};
}
static int bk_aud_adc_get_fifo_addr(uint32_t* address) {
  *address = 0x47800044u;
  return fifo_result;
}
static int bk_dma_init(dma_id_t id, const dma_config_t* config) {
  assert(id == 5);
  captured = *config;
  ++initialized;
  return init_result;
}
static int bk_dma_set_transfer_len(dma_id_t id, unsigned bytes) {
  assert(id == 5);
  transfer_bytes = bytes;
  return BK_OK;
}
static void bk_dma_set_dest_sec_attr(dma_id_t id, unsigned attr) {
  assert(id == 5 && attr == DMA_ATTR_SEC);
  ++security;
}
static void bk_dma_set_src_sec_attr(dma_id_t id, unsigned attr) {
  bk_dma_set_dest_sec_attr(id, attr);
}
static void aud_adc_dma_finish_isr(void) {}
static void bk_dma_register_isr(dma_id_t id, void* half, void* finish) {
  assert(id == 5 && !half && finish == (void*)aud_adc_dma_finish_isr);
  ++interrupts;
}
static void bk_dma_enable_finish_interrupt(dma_id_t id) {
  assert(id == 5);
  ++interrupts;
}
static void aud_adc_dma_deconfig(onboard_mic_record_priv_t* mic) {
  free(mic->dma_rb_addr);
  mic->dma_rb_addr = NULL;
  ++released;
}
#include "sdk_mic_dma_config.inc"

int main(void) {
  for (unsigned channels = 1; channels <= 2; ++channels) {
    for (unsigned frames = 1; frames <= 3; ++frames) {
      onboard_mic_record_priv_t mic = {.config = {channels},
                                       .frame_size = 320u * frames * channels,
                                       .frame_num = 2};
      initialized = interrupts = security = released = transfer_bytes = 0;
      assert(aud_adc_dma_config(&mic) == BK_OK);
      assert(initialized == 1 && interrupts == 2 && security == 2 && !released);
      assert(captured.mode == DMA_WORK_MODE_REPEAT && captured.chan_prio == 1);
      assert(captured.src.dev == DMA_DEV_AUDIO_RX &&
             captured.dst.dev == DMA_DEV_DTCM);
      assert(captured.src.addr_inc_en && captured.src.addr_loop_en);
      assert(captured.dst.addr_inc_en && captured.dst.addr_loop_en);
      assert(captured.src.start_addr == 0x47800044u &&
             captured.src.end_addr == 0x47800048u);
      assert(captured.dst.start_addr == (uint32_t)(uintptr_t)mic.dma_rb_addr);
      assert(captured.dst.end_addr - captured.dst.start_addr ==
             mic.dma_rb.bytes);
      assert(mic.dma_rb.address == (uint8_t*)mic.dma_rb_addr);
      assert(mic.dma_rb.bytes ==
             2u * mic.frame_size + DMA_CARRY_MIC_RINGBUF_SAFE_INTERVAL);
      assert(transfer_bytes == mic.frame_size);
      printf(
          "{\"channels\":%u,\"bytes\":%u,\"source_width\":%u,\"destination_width\":%u}\n",
          channels, transfer_bytes, captured.src.width, captured.dst.width);
      aud_adc_dma_deconfig(&mic);
      assert(released == 1 && !mic.dma_rb_addr);
    }
  }
  for (unsigned failure = 0; failure < 2; ++failure) {
    onboard_mic_record_priv_t mic = {
        .config = {1}, .frame_size = 640, .frame_num = 2};
    fifo_result = failure == 0 ? BK_FAIL : BK_OK;
    init_result = failure == 1 ? BK_FAIL : BK_OK;
    released = 0;
    assert(aud_adc_dma_config(&mic) == BK_FAIL);
    assert(released == 1 && !mic.dma_rb_addr);
  }
  return 0;
}
