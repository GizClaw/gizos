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
