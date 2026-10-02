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
  /* Candidate pixel-equivalent weights: per-PAL-call and per-row-block cost.
   * A call need not correspond to one physical address window. The 16-row
   * estimate is a heuristic, never a transfer constraint or timing guarantee;
   * these weights still require device validation. */
  return area(r) + 64 + 256 * ((r.bottom - r.top + 15) / 16);
}

static int append_span(h2_lua_display_plan_t *plan,
                       h2_lua_display_plan_rect_t span, int *work) {
  int best = -1, waste = 17;
  h2_lua_display_plan_rect_t merged = span;
  for (int i = plan->count - 1; i >= 0; --i) {
    if (++*work > 16384) return 0;
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
      if (++*work > 16384) return 0;
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
          (h2_lua_display_plan_rect_t){left, y, x, y + 1}, &work)) {
        plan->count = 0;
        return 0;
      }
    }
  }
  if (!coalesce(plan, gap, &work)) { plan->count = 0; return 0; }
  return 1;
}

static uint64_t plan_cost(const h2_lua_display_plan_t *plan) {
  uint64_t total = 0;
  for (int i = 0; i < plan->count; ++i) total += cost(plan->rects[i]);
  return total;
}

static h2_lua_display_plan_rect_t tile_rect(int left, int top, int right,
                                            int bottom, int width, int height) {
  return (h2_lua_display_plan_rect_t){left * 16, top * 16,
      right * 16 < width ? right * 16 : width,
      bottom * 16 < height ? bottom * 16 : height};
}

/* Preserve the legacy any-changed vertical extension. Bit 0 is immutable
 * damage and bit 1 is the consumed mark, so a winning tile candidate can be
 * rebuilt after evaluating spans without another framebuffer comparison. */
static int build_tiles(h2_lua_display_plan_t *plan, uint8_t *tiles,
                       int width, int height, int gap,
                       h2_lua_display_plan_rect_t bounds) {
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  for (int i = 0; i < columns * rows; ++i) tiles[i] &= 1;
  int left = bounds.left / 16, top = bounds.top / 16;
  int right = (bounds.right + 15) / 16, bottom = (bounds.bottom + 15) / 16;
  plan->count = 0;
  for (int ty = top; ty < bottom; ++ty) {
    for (int tx = left; tx < right; ++tx) {
      if (tiles[ty * columns + tx] != 1) continue;
      int end_x = tx + 1;
      for (int x = end_x; x < right && x - end_x <= gap; ++x)
        if (tiles[ty * columns + x] == 1) end_x = x + 1;
      int end_y = ty + 1;
      for (int y = end_y; y < bottom && y - end_y <= gap; ++y) {
        int any = 0;
        for (int x = tx; x < end_x; ++x) any |= tiles[y * columns + x] == 1;
        if (any) end_y = y + 1;
      }
      if (plan->count == H2_LUA_DISPLAY_PLAN_CAPACITY) return 0;
      plan->rects[plan->count++] = tile_rect(tx, ty, end_x, end_y, width, height);
      for (int y = ty; y < end_y; ++y)
        for (int x = tx; x < end_x; ++x) tiles[y * columns + x] |= 2;
    }
  }
  return 1;
}

int h2_lua_display_plan_select(h2_lua_display_plan_t *plan,
    const uint16_t *current, const uint16_t *previous, int width, int height,
    h2_lua_display_plan_rect_t dirty, int gap, uint8_t *tiles, int bounds) {
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  int left = columns, top = rows, right = 0, bottom = 0;
  memset(tiles, 0, (size_t)columns * rows);
  plan->count = 0;
  if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) return 0;
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
  if (right == 0) return 0;
  h2_lua_display_plan_rect_t box = tile_rect(left, top, right, bottom, width, height);
  h2_lua_display_plan_rect_t single = {0, 0, width, height};
  if (bounds || cost(box) <= cost(single)) single = box;
  uint64_t best_cost = (uint64_t)cost(single);
  int use_tiles = 0;
  if (!bounds) {
    if (build_tiles(plan, tiles, width, height, gap, box)) {
      uint64_t candidate_cost = plan_cost(plan);
      if (candidate_cost < best_cost) { best_cost = candidate_cost; use_tiles = 1; }
    }
    if (h2_lua_display_plan_build(plan, current, previous, width, dirty, gap)) {
      if (plan_cost(plan) < best_cost) return 1;
    }
    if (use_tiles) {
      /* Same immutable damage and gap: the previously bounded plan fits. */
      build_tiles(plan, tiles, width, height, gap, box);
      return 0;
    }
  }
  plan->count = 1;
  plan->rects[0] = single;
  return 0;
}
