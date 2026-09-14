#include "h2_skeleton2d.h"
#include <math.h>
#include <stdalign.h>
#include <string.h>
/* Match the fundamental alignment used by caller-owned allocation storage.
 * MSVC's C headers omit max_align_t, as in the existing app_test allocator. */
typedef union storage_alignment {
#if defined(_MSC_VER) && !defined(__clang__)
  long double floating;
  long long integer;
  void *pointer;
#else
  max_align_t value;
#endif
} storage_alignment_t;
#define VALID 0x534b3244u
#define PI 3.14159265358979323846
struct h2_skeleton2d_definition {
  unsigned magic;
  h2_skeleton2d_config_t c;
};
struct h2_skeleton2d {
  unsigned magic;
  int published, order_dirty;
  const h2_skeleton2d_definition_t *def;
  h2_skeleton2d_transform_t *pose[3], *scratch;
  h2_skeleton2d_part_state_t *parts;
  double *world, *staged_world;
  h2_skeleton2d_draw_item_t *items, *staged_items;
  size_t count;
};
static size_t aligned(size_t n) {
  size_t a = alignof(storage_alignment_t);
  return (n + a - 1) / a * a;
}
static int number(double v) { return isfinite(v) && fabs(v) <= 1e6; }
static int transform(h2_skeleton2d_transform_t v) {
  return number(v.x) && number(v.y) && number(v.angle) && number(v.sx) &&
         number(v.sy);
}
static int flag(int v) { return v == 0 || v == 1; }
static int definition(const h2_skeleton2d_definition_t *d) {
  return d && d->magic == VALID;
}
static int instance(const h2_skeleton2d_t *s) {
  return s && s->magic == VALID && definition(s->def);
}
static h2_pal_result_t validate(const h2_skeleton2d_config_t *c, size_t *nt,
                                size_t *nk) {
  *nt = *nk = 0;
  if (!c || !c->bones || c->bone_count < 1 || c->bone_count > 128 ||
      c->part_count > 256 || c->clip_count > 32 ||
      (!c->parts && c->part_count) || (!c->clips && c->clip_count))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < c->bone_count; i++)
    if (!transform(c->bones[i].local) ||
        (i == 0 ? c->bones[i].parent != SIZE_MAX : c->bones[i].parent >= i))
      return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < c->part_count; i++)
    if (c->parts[i].bone >= c->bone_count || !transform(c->parts[i].local) ||
        !flag(c->parts[i].visible))
      return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < c->clip_count; i++) {
    const h2_skeleton2d_clip_t *cl = &c->clips[i];
    unsigned char seen[128][5] = {{0}};
    if (cl->duration_us < 1 || cl->duration_us > 3600000000LL ||
        cl->track_count > 640 || (!cl->tracks && cl->track_count))
      return H2_PAL_ERR_INVALID_ARG;
    *nt += cl->track_count;
    for (size_t j = 0; j < cl->track_count; j++) {
      const h2_skeleton2d_track_t *t = &cl->tracks[j];
      if (t->bone >= c->bone_count || t->channel > 4 || !flag(t->linear) ||
          !flag(t->shortest) || !t->keys || t->key_count < 1 ||
          t->key_count > 256 || seen[t->bone][t->channel]++)
        return H2_PAL_ERR_INVALID_ARG;
      *nk += t->key_count;
      if (*nk > 16384)
        return H2_PAL_ERR_INVALID_ARG;
      for (size_t k = 0; k < t->key_count; k++)
        if (!number(t->keys[k].value) || t->keys[k].time_us < 0 ||
            t->keys[k].time_us > cl->duration_us ||
            (k && t->keys[k].time_us <= t->keys[k - 1].time_us))
          return H2_PAL_ERR_INVALID_ARG;
    }
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_definition_size(const h2_skeleton2d_config_t *c,
                                              size_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  size_t nt, nk;
  h2_pal_result_t r = validate(c, &nt, &nk);
  if (r)
    return r;
  *out = aligned(sizeof(h2_skeleton2d_definition_t)) +
         aligned(c->bone_count * sizeof(*c->bones)) +
         aligned(c->part_count * sizeof(*c->parts)) +
         aligned(c->clip_count * sizeof(*c->clips)) +
         aligned(nt * sizeof(h2_skeleton2d_track_t)) +
         aligned(nk * sizeof(h2_skeleton2d_key_t));
  return H2_PAL_OK;
}
static int overlaps(const void *a, size_t an, const void *b, size_t bn) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  return an && bn && (x <= y ? y - x < an : x - y < bn);
}
static int config_overlaps(const void *mem, size_t bytes,
                           const h2_skeleton2d_config_t *c) {
  if (overlaps(mem, bytes, c, sizeof(*c)) ||
      overlaps(mem, bytes, c->bones, c->bone_count * sizeof(*c->bones)) ||
      overlaps(mem, bytes, c->parts, c->part_count * sizeof(*c->parts)) ||
      overlaps(mem, bytes, c->clips, c->clip_count * sizeof(*c->clips)))
    return 1;
  for (size_t i = 0; i < c->clip_count; ++i) {
    const h2_skeleton2d_clip_t *clip = &c->clips[i];
    if (overlaps(mem, bytes, clip->tracks,
                 clip->track_count * sizeof(*clip->tracks)))
      return 1;
    for (size_t j = 0; j < clip->track_count; ++j)
      if (overlaps(mem, bytes, clip->tracks[j].keys,
                   clip->tracks[j].key_count * sizeof(*clip->tracks[j].keys)))
        return 1;
  }
  return 0;
}
static void *take(unsigned char **p, size_t n) {
  void *r = *p;
  *p += aligned(n);
  return r;
}
h2_pal_result_t
h2_skeleton2d_definition_init(void *mem, size_t bytes,
                              const h2_skeleton2d_config_t *c,
                              h2_skeleton2d_definition_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  size_t need, nt, nk;
  h2_pal_result_t r = h2_skeleton2d_definition_size(c, &need);
  if (r)
    return r;
  if (!mem || (uintptr_t)mem % alignof(storage_alignment_t))
    return H2_PAL_ERR_INVALID_ARG;
  if (bytes < need)
    return H2_PAL_ERR_NO_SPACE;
  if (config_overlaps(mem, need, c))
    return H2_PAL_ERR_INVALID_ARG;
  validate(c, &nt, &nk);
  unsigned char *p = mem;
  h2_skeleton2d_definition_t *d = take(&p, sizeof(*d));
  h2_skeleton2d_bone_t *bones = take(&p, c->bone_count * sizeof(*bones));
  h2_skeleton2d_part_t *parts = take(&p, c->part_count * sizeof(*parts));
  h2_skeleton2d_clip_t *clips = take(&p, c->clip_count * sizeof(*clips));
  h2_skeleton2d_track_t *tracks = take(&p, nt * sizeof(*tracks));
  h2_skeleton2d_key_t *keys = take(&p, nk * sizeof(*keys));
  memcpy(bones, c->bones, c->bone_count * sizeof(*bones));
  if (c->part_count)
    memcpy(parts, c->parts, c->part_count * sizeof(*parts));
  for (size_t i = 0; i < c->clip_count; i++) {
    clips[i] = c->clips[i];
    clips[i].tracks = tracks;
    for (size_t j = 0; j < clips[i].track_count; j++) {
      *tracks = c->clips[i].tracks[j];
      tracks->keys = keys;
      memcpy(keys, c->clips[i].tracks[j].keys,
             tracks->key_count * sizeof(*keys));
      keys += tracks->key_count;
      tracks++;
    }
  }
  d->c = *c;
  d->c.bones = bones;
  d->c.parts = parts;
  d->c.clips = clips;
  d->magic = VALID;
  *out = d;
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_instance_size(const h2_skeleton2d_definition_t *d,
                                            size_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  if (!definition(d))
    return H2_PAL_ERR_INVALID_STATE;
  *out = aligned(sizeof(h2_skeleton2d_t)) +
         4 * aligned(d->c.bone_count * sizeof(h2_skeleton2d_transform_t)) +
         aligned(d->c.part_count * sizeof(h2_skeleton2d_part_state_t)) +
         2 * aligned(d->c.bone_count * 6 * sizeof(double)) +
         2 * aligned(d->c.part_count * sizeof(h2_skeleton2d_draw_item_t));
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_instance_init(void *mem, size_t bytes,
                                            const h2_skeleton2d_definition_t *d,
                                            h2_skeleton2d_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  size_t need;
  h2_pal_result_t r = h2_skeleton2d_instance_size(d, &need);
  if (r)
    return r;
  if (!mem || (uintptr_t)mem % alignof(storage_alignment_t))
    return H2_PAL_ERR_INVALID_ARG;
  if (bytes < need)
    return H2_PAL_ERR_NO_SPACE;
  if (overlaps(mem, need, d, sizeof(*d)) || config_overlaps(mem, need, &d->c))
    return H2_PAL_ERR_INVALID_ARG;
  unsigned char *p = mem;
  h2_skeleton2d_t *s = take(&p, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->def = d;
  for (int j = 0; j < 3; j++) {
    s->pose[j] = take(&p, d->c.bone_count * sizeof(*s->pose[j]));
    for (size_t i = 0; i < d->c.bone_count; i++)
      s->pose[j][i] = d->c.bones[i].local;
  }
  s->scratch = take(&p, d->c.bone_count * sizeof(*s->scratch));
  s->parts = take(&p, d->c.part_count * sizeof(*s->parts));
  for (size_t i = 0; i < d->c.part_count; i++)
    s->parts[i] = (h2_skeleton2d_part_state_t){
        i, d->c.parts[i].resource, d->c.parts[i].layer, d->c.parts[i].visible};
  s->world = take(&p, d->c.bone_count * 6 * sizeof(double));
  s->staged_world = take(&p, d->c.bone_count * 6 * sizeof(double));
  s->items = take(&p, d->c.part_count * sizeof(*s->items));
  s->staged_items = take(&p, d->c.part_count * sizeof(*s->items));
  s->magic = VALID;
  *out = s;
  return H2_PAL_OK;
}
static double delta(double x) {
  double v = fmod(x + PI, 2 * PI);
  if (v < 0)
    v += 2 * PI;
  return v - PI;
}
static void channel(h2_skeleton2d_transform_t *t, unsigned c, double v) {
  switch (c) {
  case 0:
    t->x = v;
    break;
  case 1:
    t->y = v;
    break;
  case 2:
    t->angle = v;
    break;
  case 3:
    t->sx = v;
    break;
  default:
    t->sy = v;
  }
}
h2_pal_result_t h2_skeleton2d_sample(h2_skeleton2d_t *s, size_t id,
                                     int64_t time, h2_skeleton2d_loop_t loop,
                                     h2_skeleton2d_pose_slot_t slot) {
  if (!instance(s))
    return H2_PAL_ERR_INVALID_STATE;
  if (id >= s->def->c.clip_count || (loop != 0 && loop != 1) ||
      (slot != H2_SKELETON2D_CURRENT && slot != H2_SKELETON2D_A &&
       slot != H2_SKELETON2D_B))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_skeleton2d_clip_t *c = &s->def->c.clips[id];
  if (loop) {
    time %= c->duration_us;
    if (time < 0)
      time += c->duration_us;
  } else {
    if (time < 0)
      time = 0;
    if (time > c->duration_us)
      time = c->duration_us;
  }
  for (size_t i = 0; i < s->def->c.bone_count; i++)
    s->scratch[i] = s->def->c.bones[i].local;
  for (size_t i = 0; i < c->track_count; i++) {
    const h2_skeleton2d_track_t *t = &c->tracks[i];
    size_t lo = 0, hi = t->key_count;
    while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      if (t->keys[m].time_us <= time)
        lo = m + 1;
      else
        hi = m;
    }
    size_t k = lo ? lo - 1 : 0;
    double v = t->keys[k].value;
    if (t->linear && lo && lo < t->key_count) {
      double diff = t->keys[lo].value - v;
      if (t->channel == 2 && t->shortest)
        diff = delta(diff);
      v += diff * (double)(time - t->keys[k].time_us) /
           (double)(t->keys[lo].time_us - t->keys[k].time_us);
    }
    if (!number(v))
      return H2_PAL_ERR_INVALID_ARG;
    channel(&s->scratch[t->bone], t->channel, v);
  }
  memcpy(s->pose[slot], s->scratch, s->def->c.bone_count * sizeof(*s->scratch));
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_blend(h2_skeleton2d_t *s, double w) {
  if (!instance(s))
    return H2_PAL_ERR_INVALID_STATE;
  if (!isfinite(w) || w < 0 || w > 1)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < s->def->c.bone_count; i++) {
    h2_skeleton2d_transform_t a = s->pose[1][i], b = s->pose[2][i],
                              v = {a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w,
                                   a.angle + delta(b.angle - a.angle) * w,
                                   a.sx + (b.sx - a.sx) * w,
                                   a.sy + (b.sy - a.sy) * w};
    if (w == 0)
      v = a;
    if (w == 1)
      v = b;
    if (!transform(v))
      return H2_PAL_ERR_INVALID_ARG;
    s->scratch[i] = v;
  }
  memcpy(s->pose[0], s->scratch, s->def->c.bone_count * sizeof(*s->scratch));
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_set_local(h2_skeleton2d_t *s,
                                        const h2_skeleton2d_local_t *v,
                                        size_t n) {
  if (!instance(s))
    return H2_PAL_ERR_INVALID_STATE;
  if (n > s->def->c.bone_count || (!v && n))
    return H2_PAL_ERR_INVALID_ARG;
  unsigned char seen[128] = {0};
  for (size_t i = 0; i < n; i++)
    if (v[i].bone >= s->def->c.bone_count || !transform(v[i].local) ||
        seen[v[i].bone]++)
      return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < n; i++)
    s->pose[0][v[i].bone] = v[i].local;
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_set_parts(h2_skeleton2d_t *s,
                                        const h2_skeleton2d_part_state_t *v,
                                        size_t n) {
  if (!instance(s))
    return H2_PAL_ERR_INVALID_STATE;
  if (n > s->def->c.part_count || (!v && n))
    return H2_PAL_ERR_INVALID_ARG;
  unsigned char seen[256] = {0};
  for (size_t i = 0; i < n; i++)
    if (v[i].part >= s->def->c.part_count || !flag(v[i].visible) ||
        seen[v[i].part]++)
      return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < n; i++) {
    h2_skeleton2d_part_state_t old = s->parts[v[i].part];
    if (old.layer != v[i].layer || old.visible != v[i].visible)
      s->order_dirty = 1;
    s->parts[v[i].part] = v[i];
  }
  return H2_PAL_OK;
}
static void matrix(h2_skeleton2d_transform_t t, double *m) {
  double c = cos(t.angle), sn = sin(t.angle);
  m[0] = c * t.sx;
  m[1] = sn * t.sx;
  m[2] = -sn * t.sy;
  m[3] = c * t.sy;
  m[4] = t.x;
  m[5] = t.y;
}
static int multiply(const double *a, const double *b, double *m) {
  m[0] = a[0] * b[0] + a[2] * b[1];
  m[1] = a[1] * b[0] + a[3] * b[1];
  m[2] = a[0] * b[2] + a[2] * b[3];
  m[3] = a[1] * b[2] + a[3] * b[3];
  m[4] = a[0] * b[4] + a[2] * b[5] + a[4];
  m[5] = a[1] * b[4] + a[3] * b[5] + a[5];
  for (int i = 0; i < 6; i++)
    if (!number(m[i]))
      return 0;
  return 1;
}
static int before(h2_skeleton2d_draw_item_t a, h2_skeleton2d_draw_item_t b) {
  return a.layer < b.layer || (a.layer == b.layer && a.part < b.part);
}
static void sort(h2_skeleton2d_draw_item_t *v,
                 size_t n) { /* Bounded heap sort, O(P log P), total order
                                preserves part ties. */
  for (size_t start = n / 2; start > 0;) {
    size_t root = --start;
    for (;;) {
      size_t ch = root * 2 + 1;
      if (ch >= n)
        break;
      if (ch + 1 < n && before(v[ch], v[ch + 1]))
        ch++;
      if (!before(v[root], v[ch]))
        break;
      h2_skeleton2d_draw_item_t t = v[root];
      v[root] = v[ch];
      v[ch] = t;
      root = ch;
    }
  }
  for (size_t end = n; end > 1;) {
    h2_skeleton2d_draw_item_t t = v[0];
    v[0] = v[--end];
    v[end] = t;
    size_t root = 0;
    for (;;) {
      size_t ch = root * 2 + 1;
      if (ch >= end)
        break;
      if (ch + 1 < end && before(v[ch], v[ch + 1]))
        ch++;
      if (!before(v[root], v[ch]))
        break;
      t = v[root];
      v[root] = v[ch];
      v[ch] = t;
      root = ch;
    }
  }
}
h2_pal_result_t h2_skeleton2d_evaluate(h2_skeleton2d_t *s,
                                       const double root[6]) {
  if (!instance(s))
    return H2_PAL_ERR_INVALID_STATE;
  if (!root)
    return H2_PAL_ERR_INVALID_ARG;
  for (int i = 0; i < 6; i++)
    if (!number(root[i]))
      return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < s->def->c.bone_count; i++) {
    double m[6];
    matrix(s->pose[0][i], m);
    if (!multiply(i ? s->staged_world + 6 * s->def->c.bones[i].parent : root, m,
                  s->staged_world + 6 * i))
      return H2_PAL_ERR_INVALID_ARG;
  }
  size_t count = 0;
  int reuse_order = s->published && !s->order_dirty;
  size_t limit = reuse_order ? s->count : s->def->c.part_count;
  for (size_t at = 0; at < limit; at++) {
    size_t i = reuse_order ? s->items[at].part : at;
    const h2_skeleton2d_part_t *p = &s->def->c.parts[i];
    const h2_skeleton2d_part_state_t *st = &s->parts[i];
    if (!st->visible)
      continue;
    h2_skeleton2d_draw_item_t *item = &s->staged_items[count++];
    double m[6];
    matrix(p->local, m);
    if (!multiply(s->staged_world + 6 * p->bone, m, item->matrix))
      return H2_PAL_ERR_INVALID_ARG;
    item->part = i;
    item->resource = st->resource;
    item->layer = st->layer;
  }
  if (!reuse_order)
    sort(s->staged_items, count);
  memcpy(s->world, s->staged_world, s->def->c.bone_count * 6 * sizeof(double));
  memcpy(s->items, s->staged_items, count * sizeof(*s->items));
  s->count = count;
  s->published = 1;
  s->order_dirty = 0;
  return H2_PAL_OK;
}
h2_pal_result_t h2_skeleton2d_view(const h2_skeleton2d_t *s,
                                   h2_skeleton2d_view_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!instance(s) || !s->published)
    return H2_PAL_ERR_INVALID_STATE;
  *out = (h2_skeleton2d_view_t){s->world, s->def->c.bone_count, s->items,
                                s->count};
  return H2_PAL_OK;
}
size_t h2_skeleton2d_bone_count(const h2_skeleton2d_definition_t *d) {
  return definition(d) ? d->c.bone_count : 0;
}
size_t h2_skeleton2d_part_count(const h2_skeleton2d_definition_t *d) {
  return definition(d) ? d->c.part_count : 0;
}
void h2_skeleton2d_instance_deinit(h2_skeleton2d_t *s) {
  if (s)
    s->magic = 0;
}
void h2_skeleton2d_definition_deinit(h2_skeleton2d_definition_t *d) {
  if (d)
    d->magic = 0;
}
