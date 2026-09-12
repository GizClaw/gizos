#ifndef H2_ESP_DISPLAY_DMA_PIPELINE_H
#define H2_ESP_DISPLAY_DMA_PIPELINE_H

#include <stddef.h>

typedef struct h2_display_dma_pipeline {
    size_t slot_pixels;
    int rows;
    unsigned slots;
} h2_display_dma_pipeline_t;

static inline h2_display_dma_pipeline_t h2_display_dma_plan(size_t pixels, int width, int max_rows) {
    h2_display_dma_pipeline_t plan = {0, 0, 0};
    if (width <= 0 || max_rows <= 0 || pixels < (size_t)width) return plan;
    size_t half = (pixels / 2u) & ~(size_t)1u; /* each slot stays 4-byte aligned */
    plan.slots = half >= (size_t)width ? 2u : 1u;
    plan.slot_pixels = plan.slots == 2u ? half : pixels;
    size_t rows = plan.slot_pixels / (size_t)width;
    plan.rows = rows < (size_t)max_rows ? (int)rows : max_rows;
    return plan;
}

/* There is at most one in-flight transfer. Prepare a disjoint slot while it
 * runs, then drain before submitting the next window. A one-slot fallback
 * drains before preparation. On success no DMA references caller memory. */
static inline int h2_display_dma_run(const h2_display_dma_pipeline_t *plan, int height, void *user,
        void (*prepare)(void *, unsigned, int, int),
        int (*submit)(void *, unsigned, int, int), int (*drain)(void *)) {
    if (!plan || !plan->slots || plan->rows <= 0 || height <= 0) return -1;
    int pending = 0;
    unsigned slot = 0;
    for (int row = 0; row < height; ) {
        int rows = height - row < plan->rows ? height - row : plan->rows;
        if (pending && plan->slots == 1u) { int rc = drain(user); if (rc) return rc; pending = 0; }
        prepare(user, slot, row, rows);
        if (pending) { int rc = drain(user); if (rc) return rc; pending = 0; }
        int rc = submit(user, slot, row, rows);
        if (rc) { (void)drain(user); return rc; }
        pending = 1;
        slot = (slot + 1u) % plan->slots;
        row += rows;
    }
    return pending ? drain(user) : 0;
}
#endif
