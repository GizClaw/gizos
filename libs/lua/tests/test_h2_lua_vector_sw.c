#include "../src/modules/h2_lua_vector_sw.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h2_lua_vector_delta_cases.h"
static void *coverage_storage;
static h2_lua_vector_sw_cache_t *coverage_cache;

/* Replay every 8x8 fixture into reverse-ordered, guarded rows. This covers
 * fills, clips, transforms and malformed streams with a non-contiguous output. */
static int checked_render(const uint8_t *data,size_t length,uint8_t *rgba,
    unsigned width,unsigned height,const double matrix[6]) {
  int result=h2_lua_vector_sw_render(data,length,rgba,width,height,matrix);
  if(rgba && width==8 && height==8) {
    if(!coverage_cache) {
      size_t bytes=h2_lua_vector_sw_cache_bytes(4096);
      coverage_storage=malloc(bytes);assert(coverage_storage);
      coverage_cache=h2_lua_vector_sw_cache_init(coverage_storage,bytes);
      assert(coverage_cache);
    }
    /* Reuse across fixtures, opacity/transform changes and malformed inputs.
     * A small arena forces wrap/eviction; the second pass exercises warm hits. */
    for(unsigned pass=0;pass<2;pass++) {
      uint8_t cached[8*8*4];
      int cached_ok=h2_lua_vector_sw_render_cached_result(data,length,cached,NULL,
          width,height,matrix,NULL,NULL,coverage_cache)==H2_LUA_VECTOR_SW_OK;
      assert(result==cached_ok);
      if(result)assert(!memcmp(rgba,cached,sizeof(cached)));
    }
    uint8_t storage[8][40],*rows[8];
    memset(storage,0xa7,sizeof(storage));
    for(unsigned y=0;y<8;y++)rows[y]=storage[7-y]+4;
    int scattered=h2_lua_vector_sw_render_rows_with_poll(data,length,rows,width,height,matrix,NULL,NULL);
    assert(result==scattered);
    for(unsigned y=0;y<8;y++) {
      for(unsigned pad=0;pad<4;pad++)assert(storage[y][pad]==0xa7 && storage[y][36+pad]==0xa7);
      if(result)assert(!memcmp(rgba+y*32,rows[y],32));
    }
    rows[3]=NULL;
    assert(!h2_lua_vector_sw_render_rows_with_poll(data,length,rows,width,height,matrix,NULL,NULL));
  }
  return result;
}
#define h2_lua_vector_sw_render checked_render

static uint8_t data[65536];
static size_t length;
static void byte(unsigned v) {
  assert(length < sizeof(data));
  data[length++] = (uint8_t)v;
}
static void real(float v) {
  uint32_t n;
  memcpy(&n, &v, 4);
  for (int i = 0; i < 4; i++)
    byte(n >> (i * 8));
}
static void point(unsigned op, float x, float y) {
  byte(op);
  real(x);
  real(y);
}
static void begin(void) {
  const uint8_t header[] = {'H', '2', 'V', 'G', 8, 0, 8, 0, 0, 0, 1, 0};
  memcpy(data, header, sizeof(header));
  length = sizeof(header);
}
static void rectangle(float x, float y, float w, float h) {
  byte(4);
  point(5, x, y);
  point(6, x + w, y);
  point(6, x + w, y + h);
  point(6, x, y + h);
  byte(8);
}
static void red(void) {
  byte(10);
  byte(0);
  byte(255);
  byte(255);
  byte(255);
  byte(0);
  byte(0);
  byte(255);
  real(1);
  real(1);
}
static void test_cached_light_envelopes(void) {
  for(unsigned frame=0;frame<256;frame++) {
    begin();
    rectangle(.3f+(frame%3)*.2f,.7f,6.2f,5.8f);
    byte(11);
    for(unsigned layer=0;layer<4;layer++) {
      byte(4);point(5,1.2f,2.3f);point(6,6.7f,5.8f);
      byte(10);byte(1);byte(255);byte(255);
      byte(67+layer*37);byte(221-layer*17);byte(255);byte(255);
      real((4-layer)*.63f+(frame%17==0?.13f:0));
      real((frame+layer*19)%256/255.f);
    }
    byte(0);
    uint8_t rgba[8*8*4];
    double matrix[]={frame%31==0?-1:1,0,.07,1,frame%31==0?8:0,0};
    assert(h2_lua_vector_sw_render(data,length,rgba,8,8,matrix));
  }
}
/* Independent brute-force scan oracle: concave/self-intersecting contours,
 * edges starting above the viewport, reflection, fractional AA and clipping.
 * This intentionally visits every edge at every sample (the former cost). */
static void test_active_edges(void) {
  uint32_t seed = 9711;
  for (unsigned trial = 0; trial < 160; trial++) {
    float points[32][2];
    unsigned count = 3 + trial % 30;
    double matrix[6] = {trial & 1 ? -.7 : .9, .13, -.21, .8, 3.2, -1.1};
    begin();
    byte(4);
    for (unsigned i = 0; i < count; i++) {
      float xy[2];
      for (unsigned k = 0; k < 2; k++) {
        seed = seed * 1664525u + 1013904223u;
        xy[k] = (float)(seed % 2401) / 100 - 8;
      }
      point(i ? 6 : 5, xy[0], xy[1]);
      points[i][0] = (float)(matrix[0] * xy[0] + matrix[2] * xy[1] + matrix[4]);
      points[i][1] = (float)(matrix[1] * xy[0] + matrix[3] * xy[1] + matrix[5]);
    }
    byte(8); red(); byte(0);
    uint8_t rgba[8 * 8 * 4];
    assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, matrix));
    for (unsigned y = 0; y < 8; y++) {
      double coverage[8] = {0};
      for (unsigned sample = 0; sample < 4; sample++) {
        double sy = y + (sample + .5) / 4;
        float crossing[32]; int winds[32]; unsigned n = 0;
        for (unsigned i = 0; i < count; i++) {
          const float *a = points[i], *b = points[(i + 1) % count];
          if (fabs(b[1] - a[1]) < 1e-10 ||
              sy < fmin(a[1], b[1]) || sy >= fmax(a[1], b[1])) continue;
          float x = (float)(a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
          unsigned j = n++;
          while (j && crossing[j - 1] > x) {
            crossing[j] = crossing[j - 1]; winds[j] = winds[j - 1]; j--;
          }
          crossing[j] = x; winds[j] = b[1] > a[1] ? 1 : -1;
        }
        int winding = 0;
        for (unsigned i = 0; i < n; i++) {
          if (winding) for (unsigned x = 0; x < 8; x++)
            coverage[x] += fmax(0, fmin(x + 1., crossing[i]) -
                                     fmax(x, crossing[i - 1])) / 4;
          winding += winds[i];
        }
      }
      for (unsigned x = 0; x < 8; x++) {
        int expected = (int)floor(fmin(1, coverage[x]) * 255 + .5);
        size_t offset = (y * 8 + x) * 4;
        assert(abs((int)rgba[offset] - expected) <= 1);
        assert(abs((int)rgba[offset + 3] - expected) <= 1);
        assert(rgba[offset + 1] == 0 && rgba[offset + 2] == 0);
      }
    }
  }
}

static int stop_render(void *user) {
  unsigned *calls = user;
  return ++*calls < 3;
}
/* More than one edge block, equal start heights, a partial final block and
 * reuse for a smaller path. Repeated same-winding contours cover exactly the
 * same rectangle, independently of merge order. */
static void test_edge_blocks(void) {
  const double identity[6]={1,0,0,1,0,0};
  uint8_t expected[8*8*4],actual[8*8*4];
  begin();rectangle(1,1,6,6);red();byte(0);
  assert(h2_lua_vector_sw_render(data,length,expected,8,8,identity));
  begin();byte(4);
  for(unsigned i=0;i<1301;i++) {
    point(5,1,1);point(6,7,1);point(6,7,7);point(6,1,7);byte(8);
  }
  red();rectangle(1,1,6,6);red();byte(0);
  assert(h2_lua_vector_sw_render(data,length,actual,8,8,identity));
  assert(!memcmp(expected,actual,sizeof(actual)));
}
int main(void) {
  test_active_edges();
  test_edge_blocks();
  test_vector_delta(h2_lua_vector_sw_render);
  uint8_t rgba[8 * 8 * 4], again[sizeof(rgba)];
  double identity[] = {1, 0, 0, 1, 0, 0};
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4] == 255 && rgba[(4 * 8 + 4) * 4 + 3] == 255);
  assert(rgba[3] == 0);
  assert(h2_lua_vector_sw_render(data, length, again, 8, 8, identity));
  assert(!memcmp(rgba, again, sizeof(rgba)));
  unsigned polls = 0;
  assert(!h2_lua_vector_sw_render_with_poll(data, length, rgba, 8, 8, identity,
                                            stop_render, &polls));
  assert(polls == 3);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(!memcmp(rgba, again, sizeof(rgba)));
  for (size_t n = 0; n < length; n++)
    assert(!h2_lua_vector_sw_render(data, n, rgba, 8, 8, identity));
  /* The lower sample endpoint is inclusive, the upper endpoint exclusive.
   * Invisible-between-samples edges can be culled without reducing AA. */
  for (unsigned i = 0; i < 3; i++) {
    begin();
    rectangle(1, i == 0 ? .125f : i == 1 ? .1251f : .124f,
              6, i == 0 ? .00001f : i == 1 ? .2498f : .001f);
    red(); byte(0);
    assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
    assert(rgba[(0 * 8 + 3) * 4 + 3] == (i == 0 ? 64 : 0));
  }
  begin();
  byte(1);
  rectangle(0, 0, 4, 8);
  byte(11);
  rectangle(0, 0, 8, 8);
  red();
  byte(2);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 2) * 4 + 3] == 255 && rgba[(4 * 8 + 6) * 4 + 3] == 0);
  begin();
  for (int i = 0; i < 33; i++)
    byte(1);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(2);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(4);
  point(5, NAN, 0);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(99);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(4);
  byte(13);
  byte(4);
  byte(0);
  const unsigned xy[] = {4, 4, 28, 4, 28, 28, 4, 28};
  for (unsigned i = 0; i < 8; i++) {
    byte(xy[i]);
    byte(0);
  }
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4] == 255 && rgba[3] == 0);
  for (size_t n = 0; n < length; n++)
    assert(!h2_lua_vector_sw_render(data, n, rgba, 8, 8, identity));
  begin();
  byte(4);
  byte(13);
  byte(255);
  byte(255);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, NULL));
  double invalid_matrix[] = {NAN, 0, 0, 1, 0, 0};
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, invalid_matrix));
  invalid_matrix[0] = 0;
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, invalid_matrix));
  /* Geometry, gradient and affine operations used by the approved artwork. */
  begin();
  byte(4);
  byte(9);
  real(1);
  real(1);
  real(6);
  real(6);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4 + 3] > 240 && rgba[3] == 0);
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  double translate[] = {.5, 0, 0, .5, 4, 0};
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, translate));
  assert(rgba[(2 * 8 + 5) * 4 + 3] > 240 && rgba[(2 * 8 + 2) * 4 + 3] == 0);
  begin();
  data[8] = 1;
  byte(0);
  byte(2);
  real(0);
  real(0);
  real(1);
  real(0);
  real(0);
  byte(0);
  byte(0);
  byte(0);
  byte(255);
  real(1);
  byte(255);
  byte(255);
  byte(255);
  byte(255);
  rectangle(0, 0, 8, 8);
  byte(10);
  byte(0);
  byte(0);
  byte(0);
  for (unsigned k = 0; k < 4; k++)
    byte(255);
  real(1);
  real(1);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 1) * 4] < rgba[(4 * 8 + 6) * 4] &&
         rgba[(4 * 8 + 4) * 4 + 3] == 255);
  begin();
  byte(4);
  point(5, 1, 1);
  byte(7);
  real(1);
  real(7);
  real(7);
  real(7);
  real(7);
  real(1);
  byte(8);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(3 * 8 + 4) * 4 + 3] > 200 && rgba[3] == 0);
  begin();
  byte(4);
  point(5, 1, 4);
  point(6, 7, 4);
  byte(10);
  byte(1);
  byte(255);
  byte(255);
  byte(255);
  byte(0);
  byte(0);
  byte(255);
  real(2);
  real(1);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4 + 3] > 240 && rgba[3] == 0);
  /* Deterministic malformed-command sweep, also run under ASan/UBSan. */
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  uint32_t seed = 123;
  for (unsigned i = 0; i < 1000; i++) {
    seed = seed * 1664525u + 1013904223u;
    size_t at = seed % length;
    uint8_t saved = data[at];
    data[at] ^= (uint8_t)(1u << ((seed >> 16) % 8));
    (void)h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity);
    data[at] = saved;
  }
  puts("H2VG: visible fill, clip, determinism, truncation, stack bounds and "
       "invalid commands passed");
  test_cached_light_envelopes();
  free(coverage_storage);
  return 0;
}
