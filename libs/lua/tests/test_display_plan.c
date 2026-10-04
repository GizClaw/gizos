#include "../src/modules/h2_lua_display_plan.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef h2_lua_display_plan_rect_t rect_t;
#define MAX_PIXELS (240 * 240)
#define MAX_RECTS 225
static uint16_t previous[MAX_PIXELS], current[MAX_PIXELS], replay[MAX_PIXELS];
static rect_t legacy[MAX_RECTS], selected[MAX_RECTS], baseline[MAX_RECTS];
static h2_lua_display_plan_t plan;
static uint8_t tiles[256];

static uint64_t submission_cost(const rect_t *rects, int count, int block_cost) {
  uint64_t result = 0;
  for (int i = 0; i < count; ++i) {
    rect_t r = rects[i];
    result += (r.right-r.left)*(r.bottom-r.top) +
              block_cost*((r.bottom-r.top+15)/16);
  }
  return result;
}

/* Frozen PR627 tile planner. Only the backend submission is replaced by a
 * rectangle sink; preserve its any-changed vertical extension for comparison. */
static int old_plan(int width, int height, rect_t dirty, int gap) {
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  uint8_t changed[MAX_RECTS] = {0};
  int left = columns, top = rows, right = 0, bottom = 0, count = 0;
  for (int ty = dirty.top / 16; ty <= (dirty.bottom - 1) / 16; ++ty) {
    for (int tx = dirty.left / 16; tx <= (dirty.right - 1) / 16; ++tx) {
      int r = (tx+1)*16 < width ? (tx+1)*16 : width;
      int b = (ty+1)*16 < height ? (ty+1)*16 : height;
      int different = 0;
      for (int y = ty*16; y < b; ++y) {
        size_t at = (size_t)y*width + tx*16;
        if (memcmp(current+at, previous+at, (size_t)(r-tx*16)*2) != 0) {
          different = 1; break;
        }
      }
      if (!different) continue;
      changed[ty*columns+tx] = 1;
      if (tx < left) left = tx;
      if (ty < top) top = ty;
      if (tx+1 > right) right = tx+1;
      if (ty+1 > bottom) bottom = ty+1;
    }
  }
  for (int ty = top; ty < bottom; ++ty) {
    for (int tx = left; tx < right; ++tx) {
      if (!changed[ty*columns+tx]) continue;
      int end_x = tx+1;
      for (int x = end_x; x < right && x-end_x <= gap; ++x)
        if (changed[ty*columns+x]) end_x = x+1;
      int end_y = ty+1;
      for (int y = end_y; y < bottom && y-end_y <= gap; ++y) {
        int any = 0;
        for (int x = tx; x < end_x; ++x) any |= changed[y*columns+x];
        if (any) end_y = y+1;
      }
      for (int y = ty; y < end_y; ++y)
        memset(changed+y*columns+tx, 0, (size_t)(end_x-tx));
      assert(count < MAX_RECTS);
      legacy[count++] = (rect_t){tx*16, ty*16,
          end_x*16 < width ? end_x*16 : width,
          end_y*16 < height ? end_y*16 : height};
    }
  }
  return count;
}

static double now_us(void) {
  struct timespec t;
  assert(timespec_get(&t, TIME_UTC) == TIME_UTC);
  return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

typedef struct metrics {
  double changed, pixels[3], rects[3], blocks[3], us[3];
  double times[3][2048];
  int frames, fallback;
} metrics_t;

static void measure(metrics_t *m, int width, int height, rect_t dirty, int gap) {
  size_t size = (size_t)width*height;
  for (size_t i = 0; i < size; ++i) m->changed += current[i] != previous[i];
  ++m->frames;
  int spans = h2_lua_display_plan_build(&plan, current, previous, width, dirty, gap);
  int baseline_count = spans ? plan.count : old_plan(width, height, dirty, gap);
  memcpy(baseline, spans ? plan.rects : legacy, (size_t)baseline_count*sizeof(*baseline));
  if (!spans) ++m->fallback;
  for (int method = 0; method < 3; ++method) {
    double begin = now_us();
    const rect_t *rects = NULL;
    int count = 0;
    for (int repeat = 0; repeat < 10; ++repeat) {
      if (method == 0) {
        count = old_plan(width, height, dirty, gap); rects = legacy;
      } else {
        if (h2_lua_display_plan_select(&plan, current, previous,
                width, height, dirty, gap, tiles, 0, method == 2)) {
          count = plan.count;
          memcpy(selected, plan.rects, (size_t)count*sizeof(*selected));
        } else {
          int cursor = 0;
          count = 0;
          rect_t r;
          while (h2_lua_display_plan_next_tile(tiles, width, height, gap, &cursor, &r)) {
            assert(count < MAX_RECTS);
            selected[count++] = r;
          }
        }
        rects = selected;
      }
    }
    double elapsed = (now_us()-begin)/10;
    m->us[method] += elapsed;
    if (m->frames <= 2048) m->times[method][m->frames-1] = elapsed;
    if (method == 1) {
      /* An affine cost is no worse across the entire band iff both endpoints
       * are no worse. Compare against the complete original baseline, not a
       * different frame or an incomplete bounded tile prefix. */
      assert(submission_cost(rects, count, 222) <=
             submission_cost(baseline, baseline_count, 222));
      assert(submission_cost(rects, count, 286) <=
             submission_cost(baseline, baseline_count, 286));
      if (count != 1 || baseline_count == 1) {
        assert(count == baseline_count);
        assert(memcmp(rects, baseline, (size_t)count*sizeof(*rects)) == 0);
      }
    }
    if (method == 2) {
      /* Explicit tiles must retain the complete legacy tile plan or its
       * guarded union; it need not beat the finer default span plan. */
      int legacy_count = old_plan(width, height, dirty, gap);
      assert(submission_cost(rects, count, 222) <=
             submission_cost(legacy, legacy_count, 222));
      assert(submission_cost(rects, count, 286) <=
             submission_cost(legacy, legacy_count, 286));
      if (count != 1 || legacy_count == 1) {
        assert(count == legacy_count);
        assert(memcmp(rects, legacy, (size_t)count*sizeof(*rects)) == 0);
      }
    }
    memcpy(replay, previous, size*2);
    for (int i = 0; i < count; ++i) {
      rect_t r = rects[i];
      assert(r.left < r.right && r.top < r.bottom && r.right <= width && r.bottom <= height);
      m->pixels[method] += (r.right-r.left)*(r.bottom-r.top);
      m->blocks[method] += (r.bottom-r.top+15)/16;
      for (int y = r.top; y < r.bottom; ++y)
        memcpy(replay+(size_t)y*width+r.left, current+(size_t)y*width+r.left,
               (size_t)(r.right-r.left)*2);
    }
    m->rects[method] += count;
    assert(memcmp(replay, current, size*2) == 0);
  }
}

static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static void report(const char *name, metrics_t *m) {
  printf("%s frames=%d changed=%.1f fallback=%d scratch=%zu\n", name, m->frames,
         m->changed/m->frames, m->fallback, sizeof(plan));
  int samples = m->frames < 2048 ? m->frames : 2048;
  for (int i = 0; i < 3; ++i) {
    qsort(m->times[i], (size_t)samples, sizeof(double), compare_double);
    printf("  %s pixels=%.1f rects=%.2f blocks16=%.2f planner_us=%.2f p50=%.2f p95=%.2f max=%.2f\n",
           i == 2 ? "direct-tiles" : i ? "selected" : "tiles", m->pixels[i]/m->frames, m->rects[i]/m->frames,
           m->blocks[i]/m->frames, m->us[i]/m->frames, m->times[i][samples/2],
           m->times[i][(samples-1)*95/100], m->times[i][samples-1]);
  }
}

static uint32_t seed = 1;
static uint32_t random_u32(void) {
  seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
  return seed;
}

static void scene(int kind, int frame) {
  memset(current, 0, sizeof(current));
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      int distance = abs(x-120) + abs(y-120);
      if (kind == 0) {
        int radius = 45 + frame%60;
        if (distance >= radius && distance < radius+8) current[y*240+x] = 0xffff;
      } else if (kind == 1) {
        if (x >= frame && x < frame+50 && y >= 70 && y < 130) current[y*240+x] = 0xf800;
      } else if (kind == 2) {
        if (random_u32()%2000 == 0) current[y*240+x] = 0x07e0;
      } else if (kind == 3) current[y*240+x] = (uint16_t)random_u32();
      else if (kind == 5 && (x+y+frame)%2) current[y*240+x] = 0xffff;
    }
  }
}

int main(int argc, char **argv) {
  /* Optional local-only RGB565 big-endian full frames, 240x240, no header.
   * No consumer fixture or private rendering code belongs in this test. */
  if ((argc == 3 || argc == 5) && strcmp(argv[1], "--frames") == 0) {
    FILE *file = fopen(argv[2], "rb"); assert(file);
    static unsigned char bytes[MAX_PIXELS*2];
    metrics_t m = {0};
    int first = 1, last = 2048, frame = 0;
    if (argc == 5) {
      char *end;
      long value = strtol(argv[3], &end, 10);
      assert(*end == 0 && value >= 1 && value <= 2048); first = (int)value;
      value = strtol(argv[4], &end, 10);
      assert(*end == 0 && value >= first && value <= 2048); last = (int)value;
    }
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), file)) != 0) {
      assert(n == sizeof(bytes));
      for (size_t i = 0; i < MAX_PIXELS; ++i)
        current[i] = (uint16_t)((unsigned)bytes[2*i]*256+bytes[2*i+1]);
      ++frame;
      if (frame >= first && frame <= last)
        measure(&m, 240, 240, (rect_t){0,0,240,240}, 1);
      memcpy(previous, current, sizeof(previous));
    }
    assert(!ferror(file) && m.frames > 0); fclose(file); report("local-frames", &m);
    return 0;
  }
  assert(argc == 1);
  const char *names[] = {"hollow-diamond", "moving-rectangle", "sparse-noise",
                         "dense", "unchanged", "checkerboard"};
  for (int kind = 0; kind < 6; ++kind) {
    metrics_t m = {0};
    scene(kind, 0); memcpy(previous, current, sizeof(previous));
    for (int frame = 1; frame <= 60; ++frame) {
      scene(kind, frame);
      measure(&m, 240, 240, (rect_t){0,0,240,240}, 1);
      memcpy(previous, current, sizeof(previous));
    }
    report(names[kind], &m);
  }
  /* Odd edges, clipped dirty bounds and random old/new RGB565 changes. */
  const int sizes[][2] = {{1,1},{17,19},{31,35},{239,237}};
  for (int s = 0; s < 4; ++s) {
    int w = sizes[s][0], h = sizes[s][1];
    memset(previous, 0, sizeof(previous));
    for (int frame = 0; frame < 80; ++frame) {
      memcpy(current, previous, sizeof(current));
      int x = (int)(random_u32()%(unsigned)w), y = (int)(random_u32()%(unsigned)h);
      int r = x+1+(int)(random_u32()%(unsigned)(w-x));
      int b = y+1+(int)(random_u32()%(unsigned)(h-y));
      for (int row = y; row < b; ++row)
        for (int col = x; col < r; ++col)
          if (random_u32()%7 == 0) current[row*w+col] ^= (uint16_t)random_u32();
      metrics_t m = {0};
      measure(&m, w, h, (rect_t){x,y,r,b}, frame%9);
      memcpy(previous, current, sizeof(previous));
    }
  }
  /* Force storage exhaustion without the high-density hint, then verify the
   * coarse fallback covers every alternating pixel. */
  memset(previous, 0, sizeof(previous));
  memset(current, 0, sizeof(current));
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 240; ++x)
      if ((x+y)%2) current[y*240+x] = 0xffff;
  assert(!h2_lua_display_plan_build(&plan, current, previous, 240,
                                   (rect_t){0,0,240,240}, 0));
  assert(plan.count == 0);
  metrics_t overflow = {0};
  measure(&overflow, 240, 240, (rect_t){0,0,240,240}, 0);
  assert(overflow.fallback == 1);
  /* Maximum supported axis, one-pixel-wide/tall buffers, and the final pixel. */
  for (int vertical = 0; vertical < 2; ++vertical) {
    int w = vertical ? 1 : 4096, h = vertical ? 4096 : 1;
    memset(previous, 0, sizeof(previous));
    memset(current, 0, sizeof(current));
    current[4095] = 0xffff;
    assert(h2_lua_display_plan_build(&plan, current, previous, w,
                                    (rect_t){0,0,w,h}, 0));
    assert(plan.count == 1 && plan.rects[0].right == w && plan.rects[0].bottom == h);
    assert(plan.rects[0].left == w-1 && plan.rects[0].top == h-1);
    h2_lua_display_plan_select(&plan, current, previous, w, h,
                              (rect_t){0,0,w,h}, 0, tiles, 0, 0);
    assert(plan.count == 1 && plan.rects[0].left == w-1 && plan.rects[0].top == h-1);
    assert(h2_lua_display_plan_select(&plan, current, previous, w, h,
                                      (rect_t){0,0,w,h}, 0, tiles, 0, 1));
    assert(plan.count == 1 && plan.rects[0].right == w && plan.rects[0].bottom == h);
    assert(plan.rects[0].left == (vertical ? 0 : 4080) &&
           plan.rects[0].top == (vertical ? 4080 : 0));
  }
  /* Explicit bounds retains tile precision even when spans are much smaller. */
  memset(previous, 0, sizeof(previous));
  memset(current, 0, sizeof(current));
  current[18*31+17] = 0xffff;
  h2_lua_display_plan_select(&plan, current, previous, 31, 35,
                            (rect_t){17,18,18,19}, 0, tiles, 1, 0);
  assert(plan.count == 1 && plan.rects[0].left == 16 && plan.rects[0].top == 16);
  assert(plan.rects[0].right == 31 && plan.rects[0].bottom == 32);
  h2_lua_display_plan_t bounds_plan = plan;
  h2_lua_display_plan_select(&plan, current, previous, 31, 35,
                            (rect_t){17,18,18,19}, 0, tiles, 1, 1);
  assert(plan.count == bounds_plan.count &&
         memcmp(plan.rects, bounds_plan.rects, sizeof(rect_t)) == 0);
  h2_lua_display_plan_select(&plan, current, previous, 31, 35,
                            (rect_t){17,18,18,19}, 0, tiles, 0, 1);
  assert(plan.count == 1 &&
         memcmp(plan.rects, bounds_plan.rects, sizeof(rect_t)) == 0);
  /* Hundreds of disjoint tile runs cannot fit in the bounded plan; neither
   * exhausted tiles nor exhausted spans may publish a partial submission. */
  size_t large_count = 4096*33;
  uint16_t *old = calloc(large_count, sizeof(*old));
  uint16_t *next = calloc(large_count, sizeof(*next));
  uint8_t large_tiles[256*3];
  assert(old && next);
  for (int y = 0; y < 33; y += 32)
    for (int x = 0; x < 4096; x += 32) next[y*4096+x] = 0xffff;
  for (int direct = 0; direct < 2; ++direct) {
    memset(old, 0, large_count*sizeof(*old));
    assert(!h2_lua_display_plan_select(&plan, next, old, 4096, 33,
                                      (rect_t){0,0,4096,33}, 0, large_tiles, 0, direct));
    assert(plan.count == 0);
    int cursor = 0, count = 0;
    rect_t r;
    while (h2_lua_display_plan_next_tile(large_tiles, 4096, 33, 0, &cursor, &r)) {
      assert(r.left == (count%128)*32 && r.right == r.left+16);
      assert(r.top == (count/128)*32 && r.bottom == (count<128 ? 16 : 33));
      ++count;
      for (int y = r.top; y < r.bottom; ++y)
        memcpy(old+(size_t)y*4096+r.left, next+(size_t)y*4096+r.left,
               (size_t)(r.right-r.left)*sizeof(*old));
    }
    assert(count == 256);
    assert(memcmp(old, next, large_count*sizeof(*old)) == 0);
  }
  free(old); free(next);
  /* Guard boundary, ties and rejection preserve the completed plan exactly. */
  memset(&plan, 0, sizeof(plan));
  h2_lua_display_plan_t saved = plan;
  assert(!h2_lua_display_plan_guard(&plan));
  assert(memcmp(&plan, &saved, sizeof(plan)) == 0);
  plan.count = 2;
  plan.rects[0] = (rect_t){0,0,1,1};
  plan.rects[1] = (rect_t){223,0,224,1};
  saved = plan;
  assert(!h2_lua_display_plan_guard(&plan)); /* +222 px, -1 block: tie */
  assert(memcmp(&plan, &saved, sizeof(plan)) == 0);
  plan.rects[1] = (rect_t){222,0,223,1};
  assert(h2_lua_display_plan_guard(&plan));
  assert(plan.count == 1 && plan.rects[0].right == 223);
  saved = plan;
  assert(!h2_lua_display_plan_guard(&plan));
  assert(memcmp(&plan, &saved, sizeof(plan)) == 0);
  plan.count = 2;
  plan.rects[0] = (rect_t){0,0,1,1};
  plan.rects[1] = (rect_t){0,4095,1,4096};
  saved = plan;
  assert(!h2_lua_display_plan_guard(&plan));
  assert(memcmp(&plan, &saved, sizeof(plan)) == 0);
  /* At maximum dimensions accumulated overlapping area exceeds INT32_MAX. */
  plan.count = H2_LUA_DISPLAY_PLAN_CAPACITY;
  for (int i = 0; i < plan.count; ++i) plan.rects[i] = (rect_t){0,0,4096,4096};
  assert(h2_lua_display_plan_guard(&plan));
  assert(plan.count == 1 && plan.rects[0].right == 4096 && plan.rects[0].bottom == 4096);
  puts("display plan pixel coverage PASS");
  return 0;
}
