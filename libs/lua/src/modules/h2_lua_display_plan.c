#include "h2_lua_display_plan.h"

#include <stddef.h>
#include <string.h>

static int area(h2_lua_display_plan_rect_t r) {
  return (r.right - r.left) * (r.bottom - r.top);
}

static h2_lua_display_plan_rect_t join(h2_lua_display_plan_rect_t a,
                                      h2_lua_display_plan_rect_t b) {
  return (h2_lua_display_plan_rect_t){
      a.left < b.left ? a.left : b.left,
      a.top < b.top ? a.top : b.top,
      a.right > b.right ? a.right : b.right,
      a.bottom > b.bottom ? a.bottom : b.bottom};
}

static int cost(h2_lua_display_plan_rect_t r) {
  /* Include both window and row-block overhead without changing PAL. The
   * 16-row estimate is a planning heuristic, never a transfer constraint. */
  return area(r) + 256 + 32 * ((r.bottom - r.top + 15) / 16);
}

static int append_span(h2_lua_display_plan_t *plan,
                       h2_lua_display_plan_rect_t span) {
  int best = -1, waste = 17;
  h2_lua_display_plan_rect_t merged = span;
  for (int i = plan->count - 1; i >= 0; --i) {
    h2_lua_display_plan_rect_t r = plan->rects[i];
    if (r.bottom != span.top || r.left > span.right || r.right < span.left)
      continue;
    h2_lua_display_plan_rect_t candidate = join(r, span);
    int extra = area(candidate) - area(r) - area(span);
    /* Keep scanline growth tight. Paying a whole window for every scanline
     * would repeatedly absorb the unchanged interior of a hollow shape. */
    if (extra < waste) {
      waste = extra;
      best = i;
      merged = candidate;
    }
  }
  if (best >= 0) {
    plan->rects[best] = merged;
    return 1;
  }
  if (plan->count == H2_LUA_DISPLAY_PLAN_CAPACITY) return 0;
  plan->rects[plan->count++] = span;
  return 1;
}

static int coalesce(h2_lua_display_plan_t *plan, int gap, int *work) {
  /* One bounded pair pass, after vertical runs exist. Merge only when the
   * complete rectangle costs no more than the two separate submissions. */
  for (int i = 0; i < plan->count; ++i) {
    for (int j = i + 1; j < plan->count; ++j) {
      if (++*work > 65536) return 0;
      h2_lua_display_plan_rect_t a = plan->rects[i], b = plan->rects[j];
      if (b.left / 16 - (a.right - 1) / 16 - 1 > gap ||
          a.left / 16 - (b.right - 1) / 16 - 1 > gap ||
          b.top / 16 - (a.bottom - 1) / 16 - 1 > gap ||
          a.top / 16 - (b.bottom - 1) / 16 - 1 > gap) continue;
      h2_lua_display_plan_rect_t merged = join(a, b);
      if (cost(merged) > cost(a) + cost(b)) continue;
      plan->rects[i] = merged;
      plan->rects[j--] = plan->rects[--plan->count];
    }
  }
  return 1;
}

int h2_lua_display_plan_build(h2_lua_display_plan_t *plan,
    const uint16_t *current, const uint16_t *previous, int width,
    h2_lua_display_plan_rect_t dirty, int gap) {
  plan->count = 0;
  int spans = 0, work = 0;
  /* A density hint only selects the already-correct coarse fallback. Sampling
   * cannot omit damage. Avoid a full exact scan for solid fills / dense video. */
  if (dirty.right - dirty.left >= 16 && dirty.bottom - dirty.top >= 16 &&
      area(dirty) >= 4096) {
    int changed = 0;
    for (int row = 0; row < 8; ++row) {
      int y = dirty.top + row * (dirty.bottom - dirty.top - 1) / 7;
      for (int col = 0; col < 8; ++col) {
        int x = dirty.left + col * (dirty.right - dirty.left - 1) / 7;
        size_t at = (size_t)y * width + x;
        changed += current[at] != previous[at];
      }
    }
    if (changed >= 56) return 0;
  }
  for (int y = dirty.top; y < dirty.bottom; ++y) {
    if ((y - dirty.top) % 16 == 0 && !coalesce(plan, gap, &work)) {
      plan->count = 0;
      return 0;
    }
    const uint16_t *a = current + (size_t)y * width;
    const uint16_t *b = previous + (size_t)y * width;
    if (memcmp(a + dirty.left, b + dirty.left,
               (size_t)(dirty.right - dirty.left) * sizeof(*a)) == 0) continue;
    int x = dirty.left;
    while (x < dirty.right) {
      while (x < dirty.right && a[x] == b[x]) ++x;
      if (x == dirty.right) break;
      int left = x++;
      while (x < dirty.right && a[x] != b[x]) ++x;
      if (++spans > 4096 || !append_span(plan,
          (h2_lua_display_plan_rect_t){left, y, x, y + 1})) {
        plan->count = 0;
        return 0;
      }
    }
  }
  if (!coalesce(plan, gap, &work)) { plan->count = 0; return 0; }
  return 1;
}

typedef struct plan_summary {
  h2_lua_display_plan_rect_t box;
  int64_t pixels, blocks;
  int count;
} plan_summary_t;

static void summarize(plan_summary_t *summary, h2_lua_display_plan_rect_t r) {
  summary->box = summary->count ? join(summary->box, r) : r;
  summary->pixels += area(r);
  summary->blocks += (r.bottom - r.top + 15) / 16;
  ++summary->count;
}

static int guard(const plan_summary_t *summary) {
  if (summary->count < 2) return 0;
  int64_t pixels = area(summary->box) - summary->pixels;
  int64_t blocks = (summary->box.bottom - summary->box.top + 15) / 16 -
                   summary->blocks;
  /* Conservative empirical band, in pixel equivalents per estimated block.
   * 160..200 us/block divided by .70...72 us/pixel, rounded outward. Require
   * savings at the worst endpoint and give no credit for fewer PAL calls.
   * This is not a PAL timing contract: different backends can lie outside the
   * band or use another block height. Preserve the baseline on ties. */
  return pixels + (blocks > 0 ? 286 : 222) * blocks < 0;
}

int h2_lua_display_plan_guard(h2_lua_display_plan_t *plan) {
  plan_summary_t summary = {0};
  for (int i = 0; i < plan->count; ++i) summarize(&summary, plan->rects[i]);
  if (!guard(&summary)) return 0;
  plan->rects[0] = summary.box;
  plan->count = 1;
  return 1;
}

static h2_lua_display_plan_rect_t tile_rect(int left, int top, int right,
                                            int bottom, int width, int height) {
  return (h2_lua_display_plan_rect_t){left * 16, top * 16,
      right * 16 < width ? right * 16 : width,
      bottom * 16 < height ? bottom * 16 : height};
}

static void reset_tiles(uint8_t *tiles, int width, int height) {
  int count = ((width + 15) / 16) * ((height + 15) / 16);
  for (int i = 0; i < count; ++i) tiles[i] &= 1;
}

/* Bit 0 is immutable damage; bit 1 marks consumed tiles. Enumerating the
 * complete legacy fallback has no rectangle capacity limit. */
int h2_lua_display_plan_next_tile(uint8_t *tiles, int width, int height,
    int gap, int *cursor, h2_lua_display_plan_rect_t *rect) {
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  while (*cursor < columns * rows && tiles[*cursor] != 1) ++*cursor;
  if (*cursor == columns * rows) return 0;
  int tx = *cursor % columns, ty = *cursor / columns;
  ++*cursor;
  int end_x = tx + 1;
  for (int x = end_x; x < columns && x - end_x <= gap; ++x)
    if (tiles[ty * columns + x] == 1) end_x = x + 1;
  int end_y = ty + 1;
  for (int y = end_y; y < rows && y - end_y <= gap; ++y) {
    int any = 0;
    for (int x = tx; x < end_x; ++x) any |= tiles[y * columns + x] == 1;
    if (any) end_y = y + 1;
  }
  *rect = tile_rect(tx, ty, end_x, end_y, width, height);
  for (int y = ty; y < end_y; ++y)
    for (int x = tx; x < end_x; ++x) tiles[y * columns + x] |= 2;
  return 1;
}

int h2_lua_display_plan_select(h2_lua_display_plan_t *plan,
    const uint16_t *current, const uint16_t *previous, int width, int height,
    h2_lua_display_plan_rect_t dirty, int gap, uint8_t *tiles, int bounds, int tiles_only) {
  plan->count = 0;
  if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) return 1;
  /* Preserve the default span plan and its work limits. Explicit tiles_only
   * skips straight to the same exact tile comparison and complete fallback. */
  if (!bounds && !tiles_only && h2_lua_display_plan_build(plan, current, previous, width, dirty, gap)) {
    h2_lua_display_plan_guard(plan);
    return 1;
  }
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  int left = columns, top = rows, right = 0, bottom = 0;
  memset(tiles, 0, (size_t)columns * rows);
  for (int ty = dirty.top / 16; ty <= (dirty.bottom - 1) / 16; ++ty) {
    for (int tx = dirty.left / 16; tx <= (dirty.right - 1) / 16; ++tx) {
      h2_lua_display_plan_rect_t r = tile_rect(tx, ty, tx + 1, ty + 1, width, height);
      int different = 0;
      for (int y = r.top; y < r.bottom; ++y) {
        size_t at = (size_t)y * width + r.left;
        if (memcmp(current + at, previous + at,
                   (size_t)(r.right - r.left) * sizeof(*current)) != 0) {
          different = 1;
          break;
        }
      }
      if (!different) continue;
      tiles[ty * columns + tx] = 1;
      if (tx < left) left = tx;
      if (ty < top) top = ty;
      if (tx + 1 > right) right = tx + 1;
      if (ty + 1 > bottom) bottom = ty + 1;
    }
  }
  if (right == 0) return 1;
  if (bounds) {
    plan->count = 1;
    plan->rects[0] = tile_rect(left, top, right, bottom, width, height);
    return 1;
  }
  plan_summary_t summary = {0};
  int cursor = 0;
  h2_lua_display_plan_rect_t r;
  while (h2_lua_display_plan_next_tile(tiles, width, height, gap, &cursor, &r)) {
    if (plan->count < H2_LUA_DISPLAY_PLAN_CAPACITY) plan->rects[plan->count++] = r;
    summarize(&summary, r);
  }
  if (guard(&summary)) {
    plan->count = 1;
    plan->rects[0] = summary.box;
  } else if (summary.count > H2_LUA_DISPLAY_PLAN_CAPACITY) {
    /* Never replace an oversized complete fallback with a costlier box or
     * publish its stored prefix. Replay its byte map, not the framebuffer. */
    plan->count = 0;
    reset_tiles(tiles, width, height);
    return 0;
  }
  return 1;
}
