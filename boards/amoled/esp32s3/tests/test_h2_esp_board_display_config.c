#include "h2_esp_board.h"
#include "h2_esp_display_dma_pipeline.h"

#include <assert.h>
#include <limits.h>

typedef struct mock_dma {
    h2_display_dma_pipeline_t plan;
    uint16_t pixels[368*64];
    int width, pending, next_row, active_row, active_rows, submissions, fail_submit;
    unsigned active_slot;
} mock_dma_t;

static void prepare(void *user, unsigned slot, int row, int rows) {
    mock_dma_t *m=user;
    assert(!m->pending || m->active_slot!=slot); /* never overwrite in-flight memory */
    assert(row==m->next_row && slot<m->plan.slots);
    assert((size_t)rows*m->width<=m->plan.slot_pixels);
    for(int i=0;i<rows*m->width;++i)m->pixels[slot*m->plan.slot_pixels+i]=(uint16_t)(row+i);
}
static int submit(void *user, unsigned slot, int row, int rows) {
    mock_dma_t *m=user;assert(!m->pending);
    if(++m->submissions==m->fail_submit)return -7;
    m->pending=1;m->active_slot=slot;m->active_row=row;m->active_rows=rows;m->next_row+=rows;
    return 0;
}
static int drain(void *user) {
    mock_dma_t *m=user;
    if(m->pending)for(int i=0;i<m->active_rows*m->width;++i)
        assert(m->pixels[m->active_slot*m->plan.slot_pixels+i]==(uint16_t)(m->active_row+i));
    m->pending=0;return 0;
}

int main(void) {
    h2_esp_board_display_config_t config = {
        .pclk_hz = 0u,
        .te_timeout_ms = 0u,
        .sync_to_te = 0,
    };
    assert(h2_esp_board_display_config_is_valid(&config));
    assert(h2_esp_board_display_config_is_valid(NULL) == 0);

    config.pclk_hz = (uint32_t)INT_MAX;
    assert(h2_esp_board_display_config_is_valid(&config));
    config.pclk_hz = (uint32_t)INT_MAX + 1u;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);
    config.pclk_hz = 0u;

    config.sync_to_te = 1;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);
    config.te_timeout_ms = 34u;
    assert(h2_esp_board_display_config_is_valid(&config));
    config.sync_to_te = 2;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);
    config.sync_to_te = 0;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);
    config.te_timeout_ms = 1001u;
    config.sync_to_te = 1;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);

    assert(h2_esp_board_display_config_may_apply(0));
    assert(h2_esp_board_display_config_may_apply(1) == 0);
    for(int buffer_rows=8;buffer_rows<=64;buffer_rows*=2)for(int width=1;width<=368;++width) {
        h2_display_dma_pipeline_t plan=h2_display_dma_plan((size_t)368*buffer_rows,width,64);
        assert(plan.slots==2 && plan.slot_pixels%2==0);
        assert(plan.slot_pixels*plan.slots<=(size_t)368*buffer_rows);
        assert(plan.rows>0 && plan.rows<=64 && (size_t)plan.rows*width<=plan.slot_pixels);
    }
    const int widths[]={1,7,16,33,88,368},heights[]={1,31,32,33,447,448};
    for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);++w)
      for(unsigned h=0;h<sizeof(heights)/sizeof(heights[0]);++h) {
        mock_dma_t m={0};m.width=widths[w];m.plan=h2_display_dma_plan(368*64,m.width,64);
        assert(h2_display_dma_run(&m.plan,heights[h],&m,prepare,submit,drain)==0);
        assert(m.next_row==heights[h] && !m.pending);
      }
    mock_dma_t one={0};one.width=368;one.plan=h2_display_dma_plan(368,368,64);
    assert(one.plan.slots==1);
    assert(h2_display_dma_run(&one.plan,5,&one,prepare,submit,drain)==0 && !one.pending);
    mock_dma_t failed={0};failed.width=368;failed.fail_submit=2;failed.plan=h2_display_dma_plan(368*64,368,64);
    assert(h2_display_dma_run(&failed.plan,448,&failed,prepare,submit,drain)==-7 && !failed.pending);
    assert(h2_display_dma_plan(0,368,64).slots==0 && h2_display_dma_plan(368,0,64).slots==0);
    return 0;
}
