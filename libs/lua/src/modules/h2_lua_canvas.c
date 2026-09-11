#include "h2_lua_canvas.h"
#ifdef H2_QI_DUEL_DESKTOP_VECTORS
#ifdef H2_LUA_SOFTWARE_VECTORS
#include "h2_lua_vector_sw.h"
#define h2_lua_vector_cg_render(...) render_sw_for_lua(s, __VA_ARGS__)
#else
#include "h2_lua_vector_cg.h"
#endif
#endif

#include <math.h>
#include <string.h>
#include "zlib.h"

#ifdef H2_LUA_SOFTWARE_VECTORS
typedef struct {
  h2_lua_job_t *job;
  uint64_t last_yield_ms;
} vector_poll_t;
static int vector_poll(void *user) {
  vector_poll_t *poll = user;
  h2_lua_job_t *job = poll->job;
  uint64_t now = h2_lua_now_ms(job->host);
  if (job->cancel_requested || atomic_load(&job->host->stopping) ||
      now - job->started_ms >= job->host->config.execution_timeout_ms) return 0;
  if (now - poll->last_yield_ms >= 32) {
    /* Yield the native worker to the RTOS idle/input/audio tasks. This does
     * not yield Lua across a C boundary or expose a partially drawn frame. */
    if (h2_pal_time_sleep_ms(job->host->config.runtime->time, 1) != H2_PAL_OK) return 0;
    poll->last_yield_ms = h2_lua_now_ms(job->host);
  }
  return 1;
}
static int render_sw_cached_output_for_lua(lua_State *s,const uint8_t *data,size_t length,
    uint8_t *rgba,uint8_t *const *rows,unsigned width,unsigned height,
    const double matrix[6],h2_lua_vector_sw_cache_t *cache) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  /* Lua's heap limit does not reserve space for native raster workspaces.
   * Inputs/outputs are pinned on the stack or in the registry. Retry only an
   * allocation failure, after the failed renderer has freed every workspace. */
  if(length>=32768)lua_gc(s,LUA_GCCOLLECT,0);
  vector_poll_t poll={job,h2_lua_now_ms(job->host)};
  h2_lua_vector_sw_result_t result=h2_lua_vector_sw_render_cached_result(
      data,length,rgba,rows,width,height,matrix,vector_poll,&poll,cache);
  if(result==H2_LUA_VECTOR_SW_NO_MEMORY) {
    lua_gc(s,LUA_GCCOLLECT,0);
    poll.last_yield_ms=h2_lua_now_ms(job->host);
    result=h2_lua_vector_sw_render_cached_result(data,length,rgba,rows,width,height,
                                         matrix,vector_poll,&poll,cache);
    if(result==H2_LUA_VECTOR_SW_OK)
      (void)h2_pal_log_write(job->host->config.runtime->log,H2_PAL_LOG_INFO,
          "lua.vector","native workspace allocation recovered after Lua GC");
  }
  if(result==H2_LUA_VECTOR_SW_NO_MEMORY)
    return luaL_error(s,"native vector workspace allocation failed after Lua GC");
  if(result==H2_LUA_VECTOR_SW_INTERRUPTED)
    return luaL_error(s,"native vector render interrupted");
  if(result==H2_LUA_VECTOR_SW_WORKSPACE_LIMIT)
    return luaL_error(s,"native vector workspace limit exceeded");
  return result==H2_LUA_VECTOR_SW_OK;
}
static int render_sw_output_for_lua(lua_State *s,const uint8_t *data,size_t length,
    uint8_t *rgba,uint8_t *const *rows,unsigned width,unsigned height,
    const double matrix[6]) {
  return render_sw_cached_output_for_lua(s,data,length,rgba,rows,width,height,matrix,NULL);
}
static int render_sw_for_lua(lua_State *s,const uint8_t *data,size_t length,
    uint8_t *rgba,unsigned width,unsigned height,const double matrix[6]) {
  return render_sw_output_for_lua(s,data,length,rgba,NULL,width,height,matrix);
}
#endif

typedef struct canvas {
  int width, height, active, all_dirty;
  uint8_t rgb[];
} canvas_t;
typedef struct sprite {
  unsigned width, height;
  unsigned levels, level_width[13], level_height[13];
  size_t level_offset[13];
  uint8_t rgba[]; /* premultiplied for filtering */
} sprite_t;
static inline float canvas_minf(float a, float b) { return a < b ? a : b; }
static inline float canvas_maxf(float a, float b) { return a > b ? a : b; }
static char s_canvas_key, s_images_key, s_polygon_key, s_styles_key;

void h2_lua_canvas_reset(lua_State *s) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_canvas_key);
  canvas_t *canvas=lua_touserdata(s,-1);
  if(canvas)canvas->active=0;
  lua_pop(s,1);
}

static double finite_number(lua_State *s, int arg) {
  double value = luaL_checknumber(s, arg);
  if (!isfinite(value) || fabs(value) > 1000000.0)
    luaL_error(s, "canvas coordinate must be finite and bounded");
  return value;
}

static canvas_t *borrow_canvas(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  lua_rawgetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  canvas_t *canvas = lua_touserdata(s, -1);
  lua_pop(s, 1);
  if (!job->display_open || !canvas || !canvas->active ||
      canvas->width != job->display_info.width || canvas->height != job->display_info.height)
    luaL_error(s, "canvas composition is not open");
  return canvas;
}

/* Two compact bitsets follow RGB888: occupied tiles and edited tiles. */
static size_t canvas_tile_bytes(const canvas_t *c) {
  return ((size_t)((c->width+15)/16)*((c->height+15)/16)+7)/8;
}
static uint8_t *canvas_tiles(canvas_t *c) {return c->rgb+(size_t)c->width*c->height*3;}
static canvas_t *get_canvas(lua_State *s) {
  canvas_t *c=borrow_canvas(s);c->all_dirty=1;return c;
}
static void canvas_mark_box(canvas_t *c,int x0,int y0,int x1,int y1) {
  if(c->all_dirty || x0>=x1 || y0>=y1)return;
  unsigned columns=(unsigned)(c->width+15)/16;
  uint8_t *occupied=canvas_tiles(c),*dirty=occupied+canvas_tile_bytes(c);
  for(int y=y0/16;y<=(y1-1)/16;y++)for(int x=x0/16;x<=(x1-1)/16;x++) {
    unsigned bit=(unsigned)y*columns+(unsigned)x;
    occupied[bit/8]|=(uint8_t)(1u<<(bit%8));
    dirty[bit/8]|=(uint8_t)(1u<<(bit%8));
  }
}

static int begin_composite(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  if (!job->display_open) return luaL_error(s, "display is not open");
  int clear = 0;
  int retain = lua_type(s,1)==LUA_TSTRING && !strcmp(lua_tostring(s,1),"retain");
  if (!retain && !lua_isnoneornil(s, 1)) {
    luaL_checktype(s, 1, LUA_TBOOLEAN);
    clear = lua_toboolean(s, 1);
  }
  unsigned w = (unsigned)job->display_info.width, h = (unsigned)job->display_info.height;
  if (!w || !h || w > 4096u || h > 4096u) return luaL_error(s, "invalid canvas size");
  lua_rawgetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  canvas_t *canvas = lua_touserdata(s, -1);
  if (canvas && canvas->active) return luaL_error(s, "canvas composition already open");
  retain = retain && canvas && canvas->width==(int)w && canvas->height==(int)h;
  size_t tile_bytes=((size_t)((w+15)/16)*((h+15)/16)+7)/8;
  size_t bytes = sizeof(canvas_t) + (size_t)w * h * 3u + tile_bytes*2;
  if (!canvas || lua_rawlen(s, -1) < bytes) {
    lua_pop(s, 1);
    lua_pushnil(s); lua_rawsetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
    canvas = lua_newuserdatauv(s, bytes, 0);
    lua_pushvalue(s, -1); lua_rawsetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  }
  canvas->width = (int)w; canvas->height = (int)h; canvas->active = 1;
  canvas->all_dirty=!retain;
  if(!retain)memset(canvas_tiles(canvas),0,tile_bytes*2);
  /* Full redraw callers can clear the composition surface directly instead
   * of clearing RGB565 and then expanding that same black frame to RGB888. */
  if (clear) memset(canvas->rgb, 0, (size_t)w * h * 3);
  else if (!retain) for (size_t i = 0; i < (size_t)w * h; ++i) {
    uint16_t color = job->framebuffer[i];
    unsigned r = color >> 11u, g = (color >> 5u) & 63u, b = color & 31u;
    canvas->rgb[i*3u] = (uint8_t)((r<<3u)|(r>>2u));
    canvas->rgb[i*3u+1u] = (uint8_t)((g<<2u)|(g>>4u));
    canvas->rgb[i*3u+2u] = (uint8_t)((b<<3u)|(b>>2u));
  }
  lua_pop(s, 1);
  return 0;
}

/* Retain RGB888 between frames; convert/submit only pixels that changed in
 * the display's RGB565 representation. Existing dirty edits are unioned. */
static int end_composite(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  canvas_t *canvas = borrow_canvas(s);
  int left=canvas->width,top=canvas->height,right=-1,bottom=-1;
  unsigned columns=(unsigned)(canvas->width+15)/16,rows=(unsigned)(canvas->height+15)/16;
  uint8_t *occupied=canvas_tiles(canvas),*dirty=occupied+canvas_tile_bytes(canvas);
  for(unsigned ty=0;ty<rows;ty++)for(unsigned tx=0;tx<columns;tx++) {
    unsigned bit=ty*columns+tx;uint8_t flag=(uint8_t)(1u<<(bit%8));
    if(!canvas->all_dirty && !(dirty[bit/8]&flag))continue;
    int any=0,x1=(int)(tx*16+16),y1=(int)(ty*16+16);
    if(x1>canvas->width)x1=canvas->width;
    if(y1>canvas->height)y1=canvas->height;
    for(int y=(int)ty*16;y<y1;y++)for(int x=(int)tx*16;x<x1;x++) {
      size_t i=(size_t)y*canvas->width+x;
      const uint8_t *rgb=canvas->rgb+i*3;
      any|=rgb[0]|rgb[1]|rgb[2];
      uint16_t color=(uint16_t)((rgb[0]>>3)<<11 | (rgb[1]>>2)<<5 | (rgb[2]>>3));
      if(job->framebuffer[i]==color)continue;
      job->framebuffer[i]=color;
      if(x<left)left=x;
      if(x>right)right=x;
      if(y<top)top=y;
      if(y>bottom)bottom=y;
    }
    if(any)occupied[bit/8]|=flag;else occupied[bit/8]&=(uint8_t)~flag;
    dirty[bit/8]&=(uint8_t)~flag;
  }
  canvas->active=0;
  if(right>=left) {
    if(!job->dirty_valid) {
      job->dirty_min_x=left;job->dirty_min_y=top;
      job->dirty_max_x=right;job->dirty_max_y=bottom;
    } else {
      if(left<job->dirty_min_x)job->dirty_min_x=left;
      if(top<job->dirty_min_y)job->dirty_min_y=top;
      if(right>job->dirty_max_x)job->dirty_max_x=right;
      if(bottom>job->dirty_max_y)job->dirty_max_y=bottom;
    }
    job->dirty_valid=1;
  }
  return 0;
}
static int fade_composite(lua_State *s) {
  canvas_t *c=borrow_canvas(s);double alpha=finite_number(s,1);
  if(alpha<0 || alpha>1)return luaL_error(s,"invalid composite fade");
  uint8_t values[256];
  /* Floor guarantees even the dimmest retained pixel eventually disappears. */
  for(unsigned i=0;i<256;i++)values[i]=(uint8_t)floor(i*(1-alpha));
  unsigned columns=(unsigned)(c->width+15)/16,rows=(unsigned)(c->height+15)/16;
  uint8_t *occupied=canvas_tiles(c),*dirty=occupied+canvas_tile_bytes(c);
  for(unsigned ty=0;ty<rows;ty++)for(unsigned tx=0;tx<columns;tx++) {
    unsigned bit=ty*columns+tx;uint8_t flag=(uint8_t)(1u<<(bit%8));
    if(!c->all_dirty && !(occupied[bit/8]&flag))continue;
    dirty[bit/8]|=flag;
    int x1=(int)(tx*16+16),y1=(int)(ty*16+16);
    if(x1>c->width)x1=c->width;
    if(y1>c->height)y1=c->height;
    for(int y=(int)ty*16;y<y1;y++) {
      uint8_t *p=c->rgb+((size_t)y*c->width+tx*16)*3;
      for(int x=(int)tx*16;x<x1;x++)for(unsigned k=0;k<3;k++,p++)*p=values[*p];
    }
  }
  return 0;
}

static double color_args(lua_State *s, int arg, int alpha_arg, double rgb[3]) {
  const char *names[] = {"r","g","b"};
  luaL_checktype(s, arg, LUA_TTABLE);
  double alpha = luaL_checknumber(s, alpha_arg);
  if (!isfinite(alpha) || alpha < 0 || alpha > 1) luaL_error(s, "invalid canvas alpha");
  alpha = floor(alpha*255.0+.5)/255.0;
  for (int c=0;c<3;c++) {
    lua_getfield(s, arg, names[c]);
    double value = luaL_checknumber(s, -1); lua_pop(s,1);
    if (!isfinite(value) || value < 0 || value > 255) luaL_error(s, "invalid canvas color");
    rgb[c] = floor(value+.5)*alpha;
  }
  return alpha;
}

/* At each of the existing eight subpixel rows a capsule has one horizontal
 * interval (the union of its rectangle and round caps). Count the same 8x8
 * samples with integer intervals instead of 64 distance tests per edge pixel.
 * Keep geometry in double precision, including long off-screen segments. */
static void capsule_span(double ax, double ay, double bx, double by,
                         double radius, const double corners[4][2], double sy,
                         int width, int *first, int *end) {
  double left = width, right = 0;
  const double centers[2][2] = {{ax, ay}, {bx, by}};
  for (unsigned i = 0; i < 2; i++) {
    double dy = sy - centers[i][1];
    if (fabs(dy) <= radius) {
      double dx = sqrt(fmax(0, radius * radius - dy * dy));
      left = fmin(left, centers[i][0] - dx);
      right = fmax(right, centers[i][0] + dx);
    }
  }
  for (unsigned i = 0; i < 4; i++) {
    const double *a = corners[i], *b = corners[(i + 1) % 4];
    if (a[1] == b[1]) {
      if (sy == a[1]) {
        left = fmin(left, fmin(a[0], b[0]));
        right = fmax(right, fmax(a[0], b[0]));
      }
    } else if (sy >= fmin(a[1], b[1]) && sy <= fmax(a[1], b[1])) {
      double x = a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
      left = fmin(left, x);
      right = fmax(right, x);
    }
  }
  left = fmax(0, fmin(width, left));
  right = fmax(0, fmin(width, right));
  *first = (int)ceil(left * 8 - .5);
  *end = (int)floor(right * 8 - .5) + 1;
  if (*end < *first) *end = *first;
}

/* Ordinary screen coordinates use the hardware-friendly float path. Near
 * a sample boundary (or for distant geometry), retain the double oracle to
 * avoid unstable coverage quantization at subpixel boundaries. */
static int capsule_span_fast(const float centers[2][2], float radius,
                              const float corners[4][2], const float slopes[4],
                              float sy, int width, int *first, int *end) {
  float left = width, right = 0;
  for (unsigned i = 0; i < 2; i++) {
    float dy = sy - centers[i][1];
    if (fabsf(fabsf(dy) - radius) < .005f) return 0;
    if (fabsf(dy) <= radius) {
      float dx = sqrtf(canvas_maxf(0, radius * radius - dy * dy));
      left = canvas_minf(left, centers[i][0] - dx);
      right = canvas_maxf(right, centers[i][0] + dx);
    }
  }
  for (unsigned i = 0; i < 4; i++) {
    const float *a = corners[i], *b = corners[(i + 1) % 4];
    if (fabsf(sy - a[1]) < .002f || fabsf(sy - b[1]) < .002f) return 0;
    if (a[1] == b[1]) {
      if (sy == a[1]) {
        left = canvas_minf(left, canvas_minf(a[0], b[0]));
        right = canvas_maxf(right, canvas_maxf(a[0], b[0]));
      }
    } else if (sy >= canvas_minf(a[1], b[1]) && sy <= canvas_maxf(a[1], b[1])) {
      if (fabsf(slopes[i]) > 8) return 0;
      float x = a[0] + (sy - a[1]) * slopes[i];
      left = canvas_minf(left, x);
      right = canvas_maxf(right, x);
    }
  }
  left = canvas_maxf(0, canvas_minf(width, left)) * 8 - .5f;
  right = canvas_maxf(0, canvas_minf(width, right)) * 8 - .5f;
  if (fabsf(left - roundf(left)) < .05f ||
      fabsf(right - roundf(right)) < .05f)
    return 0;
  *first = (int)ceilf(left);
  *end = (int)floorf(right) + 1;
  if (*end < *first) *end = *first;
  return 1;
}

static void add_capsule(canvas_t *canvas, double ax, double ay, double bx, double by,
                         double radius, const double rgb[3], double over_alpha) {
  if (radius <= 0 || (rgb[0]==0 && rgb[1]==0 && rgb[2]==0)) return;
  int x0=(int)fmax(0,fmin(canvas->width,floor(fmin(ax,bx)-radius)));
  int x1=(int)fmax(0,fmin(canvas->width,ceil(fmax(ax,bx)+radius)));
  int y0=(int)fmax(0,fmin(canvas->height,floor(fmin(ay,by)-radius)));
  int y1=(int)fmax(0,fmin(canvas->height,ceil(fmax(ay,by)+radius)));
  if (x0 >= x1 || y0 >= y1) return;
  canvas_mark_box(canvas,x0,y0,x1,y1);
  double dx=bx-ax,dy=by-ay,len=hypot(dx,dy);
  double nx=len>0 ? -dy/len*radius : 0, ny=len>0 ? dx/len*radius : 0;
  const double corners[4][2]={{ax+nx,ay+ny},{bx+nx,by+ny},
                             {bx-nx,by-ny},{ax-nx,ay-ny}};
  float fast_corners[4][2], slopes[4];
  const float centers[2][2] = {{(float)ax, (float)ay}, {(float)bx, (float)by}};
  int fast = fabs(ax) <= 4096 && fabs(ay) <= 4096 &&
             fabs(bx) <= 4096 && fabs(by) <= 4096;
  for (unsigned i = 0; i < 4; i++) {
    fast_corners[i][0] = (float)corners[i][0];
    fast_corners[i][1] = (float)corners[i][1];
    double edge_dy = corners[(i + 1) % 4][1] - corners[i][1];
    slopes[i] = edge_dy == 0 ? 0 :
        (float)((corners[(i + 1) % 4][0] - corners[i][0]) / edge_dy);
  }
  /* color_args quantizes alpha to 1/255 and channels to integer RGB. */
  unsigned premul[3];
  for (unsigned c=0;c<3;c++) premul[c]=(unsigned)floor(rgb[c]*255+.5);
  unsigned alpha=over_alpha<0 ? 0 : (unsigned)floor(over_alpha*255+.5);
  for(int y=y0;y<y1;y++) {
    int first[8],end[8];
    int row_first = x1 * 8, row_end = x0 * 8;
    for(unsigned sy=0;sy<8;sy++) {
      if (!fast || !capsule_span_fast(centers, (float)radius, fast_corners,
              slopes, y + (sy + .5f) / 8, canvas->width, &first[sy], &end[sy]))
        capsule_span(ax,ay,bx,by,radius,corners,y+(sy+.5)/8,
                     canvas->width,&first[sy],&end[sy]);
      if (first[sy] < row_first) row_first = first[sy];
      if (end[sy] > row_end) row_end = end[sy];
    }
    int row_x0 = row_first / 8, row_x1 = (row_end + 7) / 8;
    if (row_x0 < x0) row_x0 = x0;
    if (row_x1 > x1) row_x1 = x1;
    for(int x=row_x0;x<row_x1;x++) {
      unsigned hit=0;
      for(unsigned sy=0;sy<8;sy++) {
        int lo=first[sy]>x*8 ? first[sy] : x*8;
        int hi=end[sy]<(x+1)*8 ? end[sy] : (x+1)*8;
        if(hi>lo) hit+=(unsigned)(hi-lo);
      }
      if(!hit) continue;
      uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3u;
      for(unsigned c=0;c<3;c++) {
        unsigned value=(pixel[c]*(255u*64u-alpha*hit)+premul[c]*hit+8160u)/16320u;
        pixel[c]=(uint8_t)(value>255u?255u:value);
      }
    }
  }
}


static int add_disc(lua_State *s) {
  canvas_t *canvas=borrow_canvas(s);
  double x=finite_number(s,1),y=finite_number(s,2),r=finite_number(s,3),rgb[3];
  if(r<0 || r>64) return luaL_error(s,"invalid canvas disc radius");
  color_args(s,4,5,rgb);
  add_capsule(canvas,x,y,x,y,r,rgb,-1);
  return 0;
}

static int add_line(lua_State *s) {
  canvas_t *canvas=borrow_canvas(s);
  double ax=finite_number(s,1),ay=finite_number(s,2),bx=finite_number(s,3),by=finite_number(s,4);
  double width=finite_number(s,5),rgb[3];
  if(width<0 || width>128) return luaL_error(s,"invalid canvas line width");
  color_args(s,6,7,rgb);
  add_capsule(canvas,ax,ay,bx,by,width*.5,rgb,-1);
  return 0;
}

/* Reusable flat command array: ax, ay, bx, by, width, r, g, b, alpha.
 * Preserve command order and the same capsule/quantization as add_line. */
static int add_lines(lua_State *s) {
  canvas_t *canvas = borrow_canvas(s);
  luaL_checktype(s, 1, LUA_TTABLE);
  lua_Integer count = luaL_checkinteger(s, 2);
  if (count < 0 || count > 8192 || lua_rawlen(s, 1) < (size_t)count * 9)
    return luaL_error(s, "invalid canvas line batch length");
  double scale = lua_isnoneornil(s, 3) ? 1 : finite_number(s, 3);
  double tx = lua_isnoneornil(s, 4) ? 0 : finite_number(s, 4);
  double ty = lua_isnoneornil(s, 5) ? 0 : finite_number(s, 5);
  if (scale <= 0) return luaL_error(s, "invalid canvas line batch scale");
  for (lua_Integer i = 0; i < count; i++) {
    double v[9];
    for (unsigned k = 0; k < 9; k++) {
      lua_rawgeti(s, 1, i * 9 + k + 1);
      v[k] = finite_number(s, -1);
      lua_pop(s, 1);
    }
    if (v[4] < 0 || v[4] * scale > 128 || v[8] < 0 || v[8] > 1)
      return luaL_error(s, "invalid canvas line batch width/alpha");
    for (unsigned k = 5; k < 8; k++)
      if (v[k] < 0 || v[k] > 255)
        return luaL_error(s, "invalid canvas line batch color");
    double ax = v[0] * scale + tx, ay = v[1] * scale + ty;
    double bx = v[2] * scale + tx, by = v[3] * scale + ty;
    if (fabs(ax) > 1000000 || fabs(ay) > 1000000 ||
        fabs(bx) > 1000000 || fabs(by) > 1000000)
      return luaL_error(s, "canvas batch coordinate out of bounds");
    double alpha = floor(v[8] * 255 + .5) / 255, rgb[3];
    for (unsigned k = 0; k < 3; k++) rgb[k] = floor(v[k + 5] + .5) * alpha;
    add_capsule(canvas, ax, ay, bx, by, v[4] * scale * .5, rgb, -1);
  }
  return 0;
}

static int over_line(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
  double ax=finite_number(s,1),ay=finite_number(s,2),bx=finite_number(s,3),by=finite_number(s,4);
  double width=finite_number(s,5),rgb[3];
  if(width<0 || width>128)return luaL_error(s,"invalid canvas line width");
  double alpha=color_args(s,6,7,rgb);
  add_capsule(canvas,ax,ay,bx,by,width*.5,rgb,alpha);
  return 0;
}

/* Thin neon rays: integrate a finite line against a Gaussian for its shadow,
 * then composite the subpixel core. Endpoint integrals avoid a rectangular halo. */
static int glow_line(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
  double ax=finite_number(s,1),ay=finite_number(s,2),bx=finite_number(s,3),by=finite_number(s,4);
  double width=finite_number(s,5),rgb[3],shadow[3];
  double alpha=color_args(s,6,7,rgb),blur=finite_number(s,8);
  color_args(s,9,7,shadow);
  if(width<0 || width>8 || blur<0 || blur>32)return luaL_error(s,"invalid glow line width/blur");
  double dx=bx-ax,dy=by-ay,len=hypot(dx,dy);
  if(alpha==0 || width==0 || len<1e-9)return 0;
  if(blur>0) {
    double sigma=blur*.5,extent=sigma*3+width*.5,denom=sqrt(2.)*sigma;
    int x0=(int)fmax(0,fmin(canvas->width,floor(fmin(ax,bx)-extent)));
    int x1=(int)fmax(0,fmin(canvas->width,ceil(fmax(ax,bx)+extent)));
    int y0=(int)fmax(0,fmin(canvas->height,floor(fmin(ay,by)-extent)));
    int y1=(int)fmax(0,fmin(canvas->height,ceil(fmax(ay,by)+extent)));
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++) {
      double px=x+.5-ax,py=y+.5-ay;
      double along=(px*dx+py*dy)/len,across=(-px*dy+py*dx)/len;
      double coverage=.25*(erf((len-along)/denom)+erf(along/denom))*
          (erf((width*.5-across)/denom)+erf((width*.5+across)/denom));
      coverage=fmax(0,fmin(1,coverage));
      uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3;
      for(int c=0;c<3;c++)pixel[c]=(uint8_t)fmin(255,floor(pixel[c]*(1-alpha*coverage)+shadow[c]*coverage+.5));
    }
  }
  add_capsule(canvas,ax,ay,bx,by,width*.5,rgb,alpha);
  return 0;
}

/* Small source-over UI polygons. Fill/stroke each cast a separable Gaussian
 * shadow; masks and scratch storage are charged to the VM, not the C stack.
 * Unlike light-atlas sprites, this geometry/blur changes continuously at runtime. */
static int draw_polygon(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
#ifdef H2_LUA_SOFTWARE_VECTORS
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  vector_poll_t poll={job,h2_lua_now_ms(job->host)};
#endif
  luaL_checktype(s,1,LUA_TTABLE);
  size_t count=lua_rawlen(s,1);
  if(count<3 || count>16)return luaL_error(s,"polygon needs 3 to 16 vertices");
  double points[16][2],minx=1e6,maxx=-1e6,miny=1e6,maxy=-1e6;
  for(size_t i=0;i<count;i++) {
    lua_rawgeti(s,1,(lua_Integer)i+1);luaL_checktype(s,-1,LUA_TTABLE);
    for(int c=0;c<2;c++){lua_rawgeti(s,-1,c+1);points[i][c]=finite_number(s,-1);lua_pop(s,1);}
    lua_pop(s,1);
    minx=fmin(minx,points[i][0]);maxx=fmax(maxx,points[i][0]);
    miny=fmin(miny,points[i][1]);maxy=fmax(maxy,points[i][1]);
  }
  double fill[3],stroke[3],shadow[3];
  double fa=color_args(s,2,3,fill),sa=color_args(s,4,5,stroke);
  double width=finite_number(s,6),sha=color_args(s,7,8,shadow),blur=finite_number(s,9);
  if(width<0 || width>8 || blur<0 || blur>32)return luaL_error(s,"invalid polygon stroke/blur");
  /* Integer-aligned, fill-only rectangles need neither supersampling nor a
   * scratch mask. Clip to the canvas so full-screen fades stay bounded by the
   * framebuffer instead of the small UI polygon raster limit. */
  if(count==4 && (sa==0 || width==0) && sha==0 &&
      minx<maxx && miny<maxy && minx==floor(minx) && maxx==floor(maxx) &&
      miny==floor(miny) && maxy==floor(maxy) &&
      points[0][0]==points[3][0] && points[1][0]==points[2][0] &&
      points[0][1]==points[1][1] && points[2][1]==points[3][1]) {
    int left=(int)fmax(0,fmin(canvas->width,minx));
    int right=(int)fmax(0,fmin(canvas->width,maxx));
    int top=(int)fmax(0,fmin(canvas->height,miny));
    int bottom=(int)fmax(0,fmin(canvas->height,maxy));
    if(fa==0)return 0;
    for(int y=top;y<bottom;y++)for(int x=left;x<right;x++) {
      uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3;
      for(int c=0;c<3;c++)pixel[c]=(uint8_t)fmin(255,floor(pixel[c]*(1-fa)+fill[c]+.5));
    }
    return 0;
  }
  int radius=(int)ceil(blur*1.5),pad=radius+(int)ceil(width*.5)+2;
  int x0=(int)floor(minx)-pad,y0=(int)floor(miny)-pad;
  int w=(int)ceil(maxx)+pad-x0,h=(int)ceil(maxy)+pad-y0;
  if(w<1 || h<1 || w>256 || h>256)return luaL_error(s,"polygon raster too large");
  if(x0>=canvas->width || y0>=canvas->height || x0+w<=0 || y0+h<=0)return 0;
  size_t pixels=(size_t)w*h,bytes=pixels*3*sizeof(float);
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_polygon_key);
  float *mask=lua_touserdata(s,-1);
  if(!mask || lua_rawlen(s,-1)<bytes) {
    lua_pop(s,1);lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_polygon_key);
    lua_gc(s,LUA_GCCOLLECT,0);
    mask=lua_newuserdatauv(s,bytes,0);lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_polygon_key);
  }
  lua_pop(s,1);
  float *tmp=mask+pixels,*soft=tmp+pixels;
  double kernel[97],total=0,sigma=fmax(.001,blur*.5);
  for(int n=-radius;n<=radius;n++){kernel[n+radius]=exp(-n*n/(2*sigma*sigma));total+=kernel[n+radius];}
  for(int n=0;n<=radius*2;n++)kernel[n]/=total;
  for(int pass=0;pass<2;pass++) {
    const double *color=pass?stroke:fill;double alpha=pass?sa:fa;
    if(alpha==0 || (pass && width==0))continue;
    memset(mask,0,pixels*sizeof(float));
    int firstx=pad-(int)ceil(width*.5)-1,lastx=w-firstx;
    int firsty=pad-(int)ceil(width*.5)-1,lasty=h-firsty;
    /* Evaluate the same 8x8 samples by horizontal spans. Fill crossings use
     * the original even/odd rule; stroke spans are unioned before counting,
     * so corners and overlapping edges never receive extra coverage. */
    for(int y=firsty;y<lasty;y++) {
#ifdef H2_LUA_SOFTWARE_VECTORS
      if(!vector_poll(&poll))return luaL_error(s,"polygon draw interrupted");
#endif
      for(int sy=0;sy<8;sy++) {
        uint8_t covered[256*8]={0};
        double py=y+y0+(sy+.5)/8;
        if(!pass) {
          double crossings[16];size_t n=0;
          for(size_t i=0,j=count-1;i<count;j=i++) {
            double ax=points[j][0],ay=points[j][1],bx=points[i][0],by=points[i][1];
            if((ay>py)!=(by>py)) {
              double cross=(bx-ax)*(py-ay)/(by-ay)+ax;
              size_t at=n++;while(at && crossings[at-1]>cross) {
                crossings[at]=crossings[at-1];at--;
              }
              crossings[at]=cross;
            }
          }
          for(size_t i=0;i+1<n;i+=2) {
            int lo=(int)ceil(fmax(firstx,fmin(lastx,crossings[i]-x0))*8-.5);
            int hi=(int)ceil(fmax(firstx,fmin(lastx,crossings[i+1]-x0))*8-.5);
            for(int k=lo;k<hi;k++)covered[k]=1;
          }
        } else {
          for(size_t i=0,j=count-1;i<count;j=i++) {
            double ax=points[j][0]-x0,ay=points[j][1];
            double bx=points[i][0]-x0,by=points[i][1];
            double dx=bx-ax,dy=by-ay,len=hypot(dx,dy),radius=width*.5;
            double nx=len>0?-dy/len*radius:0,ny=len>0?dx/len*radius:0;
            const double corners[4][2]={{ax+nx,ay+ny},{bx+nx,by+ny},
                                      {bx-nx,by-ny},{ax-nx,ay-ny}};
            int lo,hi;capsule_span(ax,ay,bx,by,radius,corners,py,w,&lo,&hi);
            if(lo<firstx*8)lo=firstx*8;
            if(hi>lastx*8)hi=lastx*8;
            for(int k=lo;k<hi;k++)covered[k]=1;
          }
        }
        for(int x=firstx;x<lastx;x++) {
          unsigned hits=0;for(int sx=0;sx<8;sx++)hits+=covered[x*8+sx];
          mask[(size_t)y*w+x]+=hits/64.f;
        }
      }
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
#ifdef H2_LUA_SOFTWARE_VECTORS
      if(x==0 && !vector_poll(&poll))return luaL_error(s,"polygon draw interrupted");
#endif
      double value=0;for(int n=-radius;n<=radius;n++)if(x+n>=0 && x+n<w)value+=mask[(size_t)y*w+x+n]*kernel[n+radius];
      tmp[(size_t)y*w+x]=(float)value;
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
#ifdef H2_LUA_SOFTWARE_VECTORS
      if(x==0 && !vector_poll(&poll))return luaL_error(s,"polygon draw interrupted");
#endif
      double value=0;for(int n=-radius;n<=radius;n++)if(y+n>=0 && y+n<h)value+=tmp[(size_t)(y+n)*w+x]*kernel[n+radius];
      soft[(size_t)y*w+x]=(float)value;
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
#ifdef H2_LUA_SOFTWARE_VECTORS
      if(x==0 && !vector_poll(&poll))return luaL_error(s,"polygon draw interrupted");
#endif
      int dx=x+x0,dy=y+y0;if(dx<0 || dy<0 || dx>=canvas->width || dy>=canvas->height)continue;
      size_t i=(size_t)y*w+x;uint8_t *pixel=canvas->rgb+((size_t)dy*canvas->width+dx)*3;
      for(int c=0;c<3;c++) {
        double value=pixel[c]*(1-soft[i]*alpha*sha)+shadow[c]*soft[i]*alpha;
        value=floor(value+.5)*(1-mask[i]*alpha)+color[c]*mask[i];
        pixel[c]=(uint8_t)fmin(255,floor(value+.5));
      }
    }
  }
  return 0;
}

#ifdef H2_QI_DUEL_DESKTOP_VECTORS
static char s_vector_scratch_key, s_vector_blend_scratch_key;
static uint8_t *vector_buffer(lua_State *s,const void *key,size_t bytes) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,key);
  if(!lua_isuserdata(s,-1)||lua_rawlen(s,-1)<bytes) {
    /* No caller retains a scratch pointer across draws. Reclaim the previous
     * surface before growing, so crossfades do not require old+new buffers. */
    lua_pop(s,1);lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,key);
    lua_gc(s,LUA_GCCOLLECT,0);lua_newuserdatauv(s,bytes,0);
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,key);
  }
  uint8_t *p=lua_touserdata(s,-1);lua_pop(s,1);return p;
}
static uint8_t *vector_scratch(lua_State *s,size_t bytes) {
  return vector_buffer(s,&s_vector_scratch_key,bytes);
}
#ifdef H2_LUA_SOFTWARE_VECTORS
static char s_vector_rows_key;
typedef struct { unsigned width,height;uint8_t *rows[]; } vector_rows_t;
/* Small Lua-owned slabs keep the two full-resolution crossfade frames usable
 * when long-lived audio buffers fragment PSRAM. The registry table owns both
 * row metadata and every slab; the renderer only borrows their addresses. */
static vector_rows_t *vector_rows(lua_State *s,unsigned w,unsigned h) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_rows_key);
  if(lua_istable(s,-1)) {
    lua_rawgeti(s,-1,1);vector_rows_t *v=lua_touserdata(s,-1);
    if(v && v->width==w && v->height==h){lua_pop(s,2);return v;}
    lua_pop(s,1);
  }
  lua_pop(s,1);lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_rows_key);
  lua_gc(s,LUA_GCCOLLECT,0);
  lua_newtable(s);int table=lua_gettop(s);
  vector_rows_t *v=lua_newuserdatauv(s,sizeof(*v)+(size_t)h*sizeof(uint8_t *),0);
  v->width=w;v->height=h;
  unsigned slab=2,rows_per_slab=16384u/(w*4u);
  if(!rows_per_slab)rows_per_slab=1;
  for(unsigned y=0;y<h;y+=rows_per_slab) {
    unsigned count=h-y<rows_per_slab?h-y:rows_per_slab;
    uint8_t *pixels=lua_newuserdatauv(s,(size_t)w*count*4,0);
    for(unsigned row=0;row<count;row++)v->rows[y+row]=pixels+(size_t)row*w*4;
    lua_rawseti(s,table,slab++);
  }
  lua_rawseti(s,table,1);
  lua_pushvalue(s,table);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_rows_key);
  lua_pop(s,1);return v;
}

#endif
static const h2_lua_resource_t *vector_resource(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));const char *name=luaL_checkstring(s,1);
  for(size_t i=0;i<job->host->config.resource_count;i++) {
    const h2_lua_resource_t *r=&job->host->config.resources[i];
    if(!strcmp(r->name,name))return r;
  }
  luaL_error(s,"unknown vector resource '%s'",name);return NULL;
}
static char s_vector_decode_key;
/* Resolution-specific render cache, not packaged artwork. The fixed arena
 * bounds retained memory; frames larger than the arena bypass it. */
#define VECTOR_CACHE_BYTES 524288u
typedef struct {
  const h2_lua_resource_t *resource;size_t offset,length,start,bytes;
  double matrix[6];int x,y,w,h;unsigned valid;
} vector_cached_t;
typedef struct {int screen_w,screen_h;unsigned next;size_t cursor,capacity;vector_cached_t slots[64];uint8_t pixels[];} vector_cache_t;
static char s_vector_cache_key;
static const uint8_t *vector_slice(lua_State *s,const h2_lua_resource_t *r,int arg,size_t *size) {
  lua_Integer off=luaL_checkinteger(s,arg),length=luaL_checkinteger(s,arg+1);
  if(off<0||length<5||(uint64_t)off>r->source_size||(uint64_t)length>r->source_size-(size_t)off)
    luaL_error(s,"invalid vector slice bounds");
  const uint8_t *p=r->source+(size_t)off;
  uint32_t bytes=p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
  if(bytes<12||bytes>524288)luaL_error(s,"invalid vector decoded size");
  /* One bounded decode buffer for every scene/frame; grow only as needed. */
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_decode_key);
  if(!lua_isuserdata(s,-1)||lua_rawlen(s,-1)<bytes) {
    size_t capacity=4096;while(capacity<bytes)capacity*=2;
    lua_pop(s,1);lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_decode_key);
    lua_gc(s,LUA_GCCOLLECT,0);lua_newuserdatauv(s,capacity,0);
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_decode_key);
  }
  uint8_t *out=lua_touserdata(s,-1);lua_pop(s,1);uLongf decoded=bytes;
  if(uncompress(out,&decoded,p+4,(uLong)length-4)!=Z_OK||decoded!=bytes)
    luaL_error(s,"invalid compressed vector path stream");
  *size=bytes;return out;
}
static void vector_bounds(const canvas_t *c,const uint8_t *data,const double m[6],int box[4]) {
  unsigned w=data[4]|(unsigned)data[5]<<8,h=data[6]|(unsigned)data[7]<<8;
  double minx=c->width,miny=c->height,maxx=0,maxy=0;
  for(int i=0;i<4;i++) {
    double x=(i&1)?w:0,y=(i&2)?h:0;
    double tx=m[0]*x+m[2]*y+m[4],ty=m[1]*x+m[3]*y+m[5];
    minx=fmin(minx,tx);maxx=fmax(maxx,tx);miny=fmin(miny,ty);maxy=fmax(maxy,ty);
  }
  box[0]=(int)fmax(0,fmin(c->width,floor(minx)-2));
  box[1]=(int)fmax(0,fmin(c->height,floor(miny)-2));
  box[2]=(int)fmax(0,fmin(c->width,ceil(maxx)+2))-box[0];
  box[3]=(int)fmax(0,fmin(c->height,ceil(maxy)+2))-box[1];
}
/* Resolution-local, lossless frame reuse. Authored keys and exact transforms
 * identify entries; alpha and frame interpolation remain live at composition.
 * A small deflate window/memory level keeps compression workspace bounded. */
static vector_cache_t *frame_cache(lua_State *s) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_cache_key);
  vector_cache_t *cache=lua_touserdata(s,-1);
  if(!cache) {
    h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
    size_t capacity=(size_t)job->display_info.width*job->display_info.height*16;
    if(capacity>VECTOR_CACHE_BYTES)capacity=VECTOR_CACHE_BYTES;
    lua_pop(s,1);cache=lua_newuserdatauv(s,sizeof(*cache)+capacity,0);
    memset(cache,0,sizeof(*cache));cache->capacity=capacity;
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_cache_key);
  }
  lua_pop(s,1);return cache;
}
/* Reserve output incrementally so a large frame can use the whole arena
 * without evicting unrelated small frames up front or allocating extra RAM. */
static int cache_output(vector_cache_t *cache,z_stream *z,size_t start) {
  if(z->avail_out)return 1;
  size_t at=start+z->total_out;
  if(at>=cache->capacity)return 0;
  size_t bytes=cache->capacity-at;
  if(bytes>4096)bytes=4096;
  for(unsigned i=0;i<64;i++) {
    vector_cached_t *v=&cache->slots[i];
    if(v->valid && v->start<at+bytes && at<v->start+v->bytes)v->valid=0;
  }
  z->next_out=cache->pixels+at;z->avail_out=(uInt)bytes;return 1;
}
/* Compressed source pixels and geometry intermediates share one bounded
 * arena. Geometry entries use SIZE_MAX, outside every valid resource slice. */
static int cached_pixels(vector_cache_t *cache,const h2_lua_resource_t *resource,
    size_t offset,size_t length,unsigned w,unsigned h,const double matrix[6],
    uint8_t *rgba,uint8_t *const *rows) {
  for(unsigned i=0;i<64;i++) {
    vector_cached_t *v=&cache->slots[i];
    if(!v->valid || v->resource!=resource || v->offset!=offset ||
       v->length!=length || v->w!=(int)w || v->h!=(int)h ||
       memcmp(v->matrix,matrix,sizeof(v->matrix)))continue;
    z_stream z={0};
    if(inflateInit2(&z,9)!=Z_OK)return -1;
    z.next_in=cache->pixels+v->start;z.avail_in=(uInt)v->bytes;
    int status=Z_OK;
    for(unsigned y=0;y<h && status==Z_OK;y++) {
      z.next_out=rows?rows[y]:rgba+(size_t)y*w*4;z.avail_out=w*4;
      status=inflate(&z,Z_NO_FLUSH);
      if(z.avail_out)break;
    }
    if(status==Z_OK && z.total_out==(size_t)w*h*4) {
      uint8_t extra;z.next_out=&extra;z.avail_out=1;
      status=inflate(&z,Z_FINISH);
    }
    int ok=status==Z_STREAM_END && z.total_out==(size_t)w*h*4;
    inflateEnd(&z);return ok?1:-1;
  }
  return 0;
}
static void cache_pixels(vector_cache_t *cache,const h2_lua_resource_t *resource,
    size_t offset,size_t length,unsigned w,unsigned h,const double matrix[6],
    uint8_t *rgba,uint8_t *const *rows) {
  if(cache->cursor>=cache->capacity)cache->cursor=0;
  size_t start=cache->cursor;
  for(unsigned attempt=0;attempt<2;attempt++) {
    z_stream z={0};
    if(deflateInit2(&z,1,Z_DEFLATED,9,1,Z_DEFAULT_STRATEGY)!=Z_OK)return;
    int status=Z_OK;
    for(unsigned y=0;y<h && status==Z_OK;y++) {
      z.next_in=rows?rows[y]:rgba+(size_t)y*w*4;z.avail_in=w*4;
      while(z.avail_in && status==Z_OK) {
        if(!cache_output(cache,&z,start)){status=Z_BUF_ERROR;break;}
        status=deflate(&z,Z_NO_FLUSH);
      }
    }
    while(status==Z_OK) {
      if(!cache_output(cache,&z,start)){status=Z_BUF_ERROR;break;}
      status=deflate(&z,Z_FINISH);
    }
    size_t bytes=z.total_out;deflateEnd(&z);
    cache->cursor=start+bytes;
    if(status==Z_STREAM_END) {
      vector_cached_t *v=&cache->slots[cache->next++%64];
      *v=(vector_cached_t){.resource=resource,.offset=offset,.length=length,
        .start=start,.bytes=bytes,.w=(int)w,.h=(int)h,.valid=1};
      memcpy(v->matrix,matrix,sizeof(v->matrix));break;
    }
    if(!start)break; /* Larger than the entire budget: render without caching. */
    start=0; /* Retry the same generated pixels across the arena wrap. */
  }
  return;
}
static int cached_vector_frame(lua_State *s,const h2_lua_resource_t *resource,
    int arg,const uint8_t *data,size_t length,unsigned w,unsigned h,
    const double matrix[6],uint8_t *rgba,uint8_t *const *rows) {
  vector_cache_t *cache=frame_cache(s);
  size_t offset=(size_t)luaL_checkinteger(s,arg),source_length=(size_t)luaL_checkinteger(s,arg+1);
  int found=cached_pixels(cache,resource,offset,source_length,w,h,matrix,rgba,rows);
  if(found)return found>0;
  if(!data)data=vector_slice(s,resource,arg,&length);
  int ok;
#ifdef H2_LUA_SOFTWARE_VECTORS
  if(rows) {
    ok=render_sw_output_for_lua(s,data,length,NULL,rows,w,h,matrix);
  } else
#endif
    ok=h2_lua_vector_cg_render(data,length,rgba,w,h,matrix);
  if(!ok)return 0;
  cache_pixels(cache,resource,offset,source_length,w,h,matrix,rgba,rows);
  return 1;
}

/* Two path keyframes are interpolated in premultiplied RGBA, matching the
 * original atlas operation. Only transient framebuffers contain pixels. */
static int draw_vector_slice(lua_State *s) {
  canvas_t *c=get_canvas(s);const h2_lua_resource_t *r=vector_resource(s);
  double m[6];for(int i=0;i<6;i++)m[i]=finite_number(s,i+4);
  double opacity=lua_isnoneornil(s,10)?1:finite_number(s,10);
  double mix=lua_isnoneornil(s,13)?0:finite_number(s,13);
  if(opacity<0||opacity>1||mix<0||mix>1||fabs(m[0]*m[3]-m[1]*m[2])<1e-12)
    return luaL_error(s,"invalid vector slice transform or blend");
  size_t n=0;int box[]={0,0,c->width,c->height};double local[6];memcpy(local,m,sizeof(m));
  const uint8_t *data=NULL;
  /* Opt-in for tiled paths contained in their viewbox. Moving streaks need
   * only their affected rectangle, not a full-screen clear/composite. */
  if(mix==0&&lua_toboolean(s,15)) {
    data=vector_slice(s,r,2,&n);
    if(n<12||memcmp(data,"H2VG",4))return luaL_error(s,"invalid vector tile header");
    vector_bounds(c,data,m,box);if(box[2]<=0||box[3]<=0)return 0;
    local[4]-=box[0];local[5]-=box[1];
  }
  size_t bytes=(size_t)box[2]*box[3]*4;
  uint8_t *rgba=vector_scratch(s,bytes),*other=NULL;
  if(!cached_vector_frame(s,r,2,data,n,(unsigned)box[2],(unsigned)box[3],local,rgba,NULL))return luaL_error(s,"invalid vector slice commands");
#ifdef H2_LUA_SOFTWARE_VECTORS
  vector_rows_t *second=NULL;
#endif
  if(mix>0) {
    data=NULL;n=0;
#ifdef H2_LUA_SOFTWARE_VECTORS
    second=vector_rows(s,(unsigned)box[2],(unsigned)box[3]);
    if(!cached_vector_frame(s,r,11,data,n,(unsigned)box[2],(unsigned)box[3],local,NULL,second->rows))return luaL_error(s,"invalid vector blend commands");
#else
    other=vector_buffer(s,&s_vector_blend_scratch_key,bytes);
    if(!cached_vector_frame(s,r,11,data,n,(unsigned)box[2],(unsigned)box[3],local,other,NULL))return luaL_error(s,"invalid vector blend commands");
#endif
  }
  /* ESP32-S3 has a float FPU. Reuse channel factors and fall back to the
   * original double expression near rounding boundaries to preserve pixels. */
  float source_gain[256],dest_gain[256];
  for(unsigned i=0;i<256;i++) {
    source_gain[i]=(float)(i*opacity);
    dest_gain[i]=(float)(1-i*opacity/255.);
  }
  for(int y=0;y<box[3];y++) {
    uint8_t *row=rgba?rgba+(size_t)y*box[2]*4:NULL;
    const uint8_t *blend=other?other+(size_t)y*box[2]*4:NULL;
#ifdef H2_LUA_SOFTWARE_VECTORS
    if(second)blend=second->rows[y];
#endif
    if(mix==.5)for(size_t i=0;i<(size_t)box[2]*4;i++)row[i]=(uint8_t)(((unsigned)row[i]+blend[i]+1)/2);
    else if(mix>0)for(size_t i=0;i<(size_t)box[2]*4;i++) {
      float value=row[i]*(float)(1-mix)+blend[i]*(float)mix;
      float rounded=floorf(value+.5f);
      row[i]=(uint8_t)(fabsf(value-(rounded-.5f))<.001f ?
          floor(row[i]*(1-mix)+blend[i]*mix+.5) : rounded);
    }
    for(int x=0;x<box[2];x++) {
      size_t p=(size_t)x*4;if(!row[p+3])continue;
      size_t q=((size_t)(y+box[1])*c->width+x+box[0])*3;
      for(int k=0;k<3;k++) {
        float value=source_gain[row[p+k]]+c->rgb[q+k]*dest_gain[row[p+3]];
        float rounded=floorf(value+.5f);
        if(fabsf(value-(rounded-.5f))<.001f) {
          double a=row[p+3]*opacity/255.;
          rounded=(float)floor(row[p+k]*opacity+c->rgb[q+k]*(1-a)+.5);
        }
        c->rgb[q+k]=(uint8_t)canvas_minf(255,rounded);
      }
    }
  }
  return 0;
}
/* Explicit preparation keeps expensive vector rasterization out of moving
 * affine draws. These pixels exist only for this Lua job, never in firmware
 * resources. A two-pixel transparent gutter preserves filtered edge coverage. */
#define VECTOR_PREPARED_BYTES (1536u * 1024u)
static char s_vector_prepared_key;
typedef struct {
  unsigned width, height, view_w, view_h;
  int screen_w, screen_h;
  size_t compressed_bytes;
  uint8_t pixels[]; /* lossless zlib stream, expanded into reusable scratch */
} prepared_vector_t;
static int prepare_vector(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  const h2_lua_resource_t *r=vector_resource(s);
  lua_Integer w=luaL_checkinteger(s,2),h=luaL_checkinteger(s,3);
  if(!job->display_open || w<1 || h<1 || w>1024 || h>1024)
    return luaL_error(s,"invalid vector preparation size");
  if(r->source_size<12 || memcmp(r->source,"H2VG",4))
    return luaL_error(s,"invalid prepared vector header");
  unsigned vw=r->source[4]|(unsigned)r->source[5]<<8;
  unsigned vh=r->source[6]|(unsigned)r->source[7]<<8;
  if(!vw || !vh)return luaL_error(s,"invalid prepared vector viewbox");
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_canvas_key);
  canvas_t *canvas=lua_touserdata(s,-1);
  if(canvas && canvas->active)return luaL_error(s,"cannot prepare vector during composition");
  lua_pop(s,1);
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_prepared_key);
  if(lua_isnil(s,-1)) {
    lua_pop(s,1);lua_newtable(s);lua_pushvalue(s,-1);
    lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_prepared_key);
  }
  int table=lua_gettop(s);
  lua_rawgetp(s,table,r);
  prepared_vector_t *existing=lua_touserdata(s,-1);
  if(existing && existing->width==(unsigned)w+4 && existing->height==(unsigned)h+4 &&
      existing->screen_w==job->display_info.width && existing->screen_h==job->display_info.height) {
    lua_pushinteger(s,(lua_Integer)lua_rawlen(s,-1));return 1;
  }
  lua_pop(s,1);
  size_t used=0,pixel_bytes=((size_t)w+4)*((size_t)h+4)*4;
  size_t bytes=sizeof(prepared_vector_t)+pixel_bytes;
  lua_pushnil(s);
  while(lua_next(s,table)) { used+=lua_rawlen(s,-1);lua_pop(s,1); }
  /* Do not evict a working entry before a replacement has rendered. Account
   * for both entries conservatively in the cache budget; callers can always use the uncached draw path. */
  if(bytes>VECTOR_PREPARED_BYTES || used>VECTOR_PREPARED_BYTES-bytes) {
    lua_pushnil(s);lua_pushliteral(s,"vector preparation budget exceeded");return 2;
  }
  /* Compress only the generated pixels, losslessly. Keeping full RGBA bodies
   * resident starves later icon/path rasterization on the 8 MiB PSRAM board. */
  uint8_t *pixels=lua_newuserdatauv(s,pixel_bytes,0);
  double m[6]={(double)w/vw,0,0,(double)h/vh,2,2};
  if(!h2_lua_vector_cg_render(r->source,r->source_size,pixels,(unsigned)w+4,(unsigned)h+4,m))
    return luaL_error(s,"prepared vector rendering failed");
  uLongf compressed_bytes=compressBound((uLong)pixel_bytes);
  uint8_t *compressed=lua_newuserdatauv(s,(size_t)compressed_bytes,0);
  if(compress2(compressed,&compressed_bytes,pixels,(uLong)pixel_bytes,1)!=Z_OK)
    return luaL_error(s,"prepared vector compression failed");
  bytes=sizeof(prepared_vector_t)+(size_t)compressed_bytes;
  if(bytes>VECTOR_PREPARED_BYTES || used>VECTOR_PREPARED_BYTES-bytes) {
    lua_settop(s,table);lua_gc(s,LUA_GCCOLLECT,0);
    lua_pushnil(s);lua_pushliteral(s,"vector preparation budget exceeded");return 2;
  }
  prepared_vector_t *v=lua_newuserdatauv(s,bytes,0);
  *v=(prepared_vector_t){.width=(unsigned)w+4,.height=(unsigned)h+4,
      .view_w=vw,.view_h=vh,.screen_w=job->display_info.width,.screen_h=job->display_info.height,
      .compressed_bytes=(size_t)compressed_bytes};
  memcpy(v->pixels,compressed,(size_t)compressed_bytes);
  lua_rawsetp(s,table,r);
  lua_settop(s,table);lua_gc(s,LUA_GCCOLLECT,0);
  lua_pushinteger(s,(lua_Integer)bytes);return 1;
}
static prepared_vector_t *prepared_vector(lua_State *s,const h2_lua_resource_t *r,
    const canvas_t *c,const double m[6]) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_prepared_key);
  if(lua_isnil(s,-1)){lua_pop(s,1);return NULL;}
  lua_rawgetp(s,-1,r);prepared_vector_t *v=lua_touserdata(s,-1);
  lua_pop(s,2);
  if(!v || v->screen_w!=c->width || v->screen_h!=c->height)return NULL;
  /* Bound both row and column sums, hence the largest singular value: even
   * shear must retain two prepared pixels per destination pixel. Larger
   * transforms fall back to vector rasterization. */
  double sx=(double)v->view_w/(v->width-4),sy=(double)v->view_h/(v->height-4);
  double norm=fmax(fabs(m[0]*sx)+fabs(m[2]*sy),fabs(m[1]*sx)+fabs(m[3]*sy));
  norm=fmax(norm,fmax((fabs(m[0])+fabs(m[1]))*sx,(fabs(m[2])+fabs(m[3]))*sy));
  return norm<=.5 ? v : NULL;
}
static int composite_prepared_vector(lua_State *s,canvas_t *c,const prepared_vector_t *v,
    const double m[6],double opacity,int arm_fade) {
  size_t pixel_bytes=(size_t)v->width*v->height*4;
  uint8_t *pixels=vector_scratch(s,pixel_bytes);
  uLongf decoded=(uLongf)pixel_bytes;
  if(uncompress(pixels,&decoded,v->pixels,(uLong)v->compressed_bytes)!=Z_OK || decoded!=pixel_bytes)
    return luaL_error(s,"invalid prepared vector pixels");
  double determinant=m[0]*m[3]-m[1]*m[2];
  float ix=(float)(m[3]/determinant),jx=(float)(-m[2]/determinant);
  float iy=(float)(-m[1]/determinant),jy=(float)(m[0]/determinant);
  float sx=(float)(v->width-4)/v->view_w,sy=(float)(v->height-4)/v->view_h;
  float alpha=(float)opacity,translate_x=(float)m[4],translate_y=(float)m[5];
  double minx=c->width,miny=c->height,maxx=0,maxy=0;
  for(unsigned corner=0;corner<4;corner++) {
    double x=(corner&1)?v->view_w:0,y=(corner&2)?v->view_h:0;
    double dx=m[0]*x+m[2]*y+m[4],dy=m[1]*x+m[3]*y+m[5];
    minx=fmin(minx,dx);maxx=fmax(maxx,dx);miny=fmin(miny,dy);maxy=fmax(maxy,dy);
  }
  int x0=(int)fmax(0,fmin(c->width,floor(minx)-2));
  int x1=(int)fmax(0,fmin(c->width,ceil(maxx)+2));
  int y0=(int)fmax(0,fmin(c->height,floor(miny)-2));
  int y1=(int)fmax(0,fmin(c->height,ceil(maxy)+2));
  float sample_dx[4],sample_dy[4];
  for(unsigned sample=0;sample<4;sample++) {
    float x=(sample&1)?.25f:-.25f,y=(sample&2)?.25f:-.25f;
    sample_dx[sample]=(ix*x+jx*y)*sx;sample_dy[sample]=(iy*x+jy*y)*sy;
  }
#ifdef H2_LUA_SOFTWARE_VECTORS
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  vector_poll_t poll={job,h2_lua_now_ms(job->host)};
#endif
  /* Scan clipped destination rows. Object-space coordinates also preserve
   * the original per-destination-pixel wrist/arm fade exactly in position. */
  for(int y=y0;y<y1;y++) {
#ifdef H2_LUA_SOFTWARE_VECTORS
    if(!(y&15) && !vector_poll(&poll))return luaL_error(s,"prepared vector draw interrupted");
#endif
    float dy=(float)y+.5f-translate_y;
    for(int x=x0;x<x1;x++) {
      float dx=(float)x+.5f-translate_x;
      float ox=ix*dx+jx*dy,oy=iy*dx+jy*dy;
      float px=ox*sx+1.5f,py=oy*sy+1.5f;
      if(px< -1 || py< -1 || px>=v->width || py>=v->height)continue;
      unsigned out[4]={0};
      /* Integrate four destination subpixels. A single center sample aliases
       * at minification, even when the prepared source itself is oversampled. */
      for(unsigned sample=0;sample<4;sample++) {
        float sample_x=px+sample_dx[sample],sample_y=py+sample_dy[sample];
        if(sample_x<0 || sample_y<0 || sample_x>=v->width-1 || sample_y>=v->height-1)continue;
        unsigned xx=(unsigned)sample_x,yy=(unsigned)sample_y;
        unsigned u=(unsigned)((sample_x-xx)*256),t=(unsigned)((sample_y-yy)*256);
        unsigned weights[4]={(256-u)*(256-t),u*(256-t),(256-u)*t,u*t};
        size_t at=((size_t)yy*v->width+xx)*4;
        const uint8_t *samples[4]={pixels+at,pixels+at+4,
            pixels+at+v->width*4,pixels+at+v->width*4+4};
        for(int j=0;j<4;j++)for(int k=0;k<4;k++)out[k]+=samples[j][k]*weights[j];
      }
      if(!out[3])continue;
      float coverage=alpha;
      if(arm_fade) {
        float fx=ox*145/v->view_w,fy=oy*177/v->view_h;
        float start=arm_fade==1?91:54,vx=arm_fade==1?-99:99;
        float t=((fx-start)*vx+(fy-101)*80)/(99*99+80*80);
        float fade=t<.48f?1:t<.8f?1-(t-.48f)/.32f*.78f:canvas_maxf(0,.22f*(1-t)/.2f);
        coverage*=fade;
      }
      float a=out[3]*(coverage/(262144.f*255));
      uint8_t *dest=c->rgb+((size_t)y*c->width+x)*3;
      for(int k=0;k<3;k++)dest[k]=(uint8_t)canvas_minf(255,
          floorf(out[k]*(coverage/262144.f)+dest[k]*(1-a)+.5f));
    }
  }
  return 0;
}
static int draw_vector_affine(lua_State *s) {
  canvas_t *c=get_canvas(s);const h2_lua_resource_t *r=vector_resource(s);double m[6];
  for(int i=0;i<6;i++)m[i]=finite_number(s,i+2);
  double opacity=lua_isnoneornil(s,8)?1:finite_number(s,8);
  if(opacity<0||opacity>1||fabs(m[0]*m[3]-m[1]*m[2])<1e-12)return luaL_error(s,"invalid vector transform");
  int arm_fade=lua_isnoneornil(s,9)?0:(int)luaL_checkinteger(s,9);
  if(arm_fade<0||arm_fade>2)return luaL_error(s,"invalid vector arm fade");
  prepared_vector_t *prepared=prepared_vector(s,r,c,m);
  if(prepared)return composite_prepared_vector(s,c,prepared,m,opacity,arm_fade);
  uint8_t *rgba=vector_scratch(s,(size_t)c->width*c->height*4);
  if(!h2_lua_vector_cg_render(r->source,r->source_size,rgba,c->width,c->height,m))return luaL_error(s,"invalid vector commands");
  for(size_t p=0;p<(size_t)c->width*c->height;p++)if(rgba[p*4+3]) {
    double coverage=opacity;
    if(arm_fade) {
      double dx=(p%(size_t)c->width)+.5-m[4],dy=(p/(size_t)c->width)+.5-m[5];
      double determinant=m[0]*m[3]-m[1]*m[2];
      unsigned vw=r->source[4]|(unsigned)r->source[5]<<8,vh=r->source[6]|(unsigned)r->source[7]<<8;
      double x=(m[3]*dx-m[2]*dy)/determinant*145/vw,y=(-m[1]*dx+m[0]*dy)/determinant*177/vh;
      double sx=arm_fade==1?91:54,vx=arm_fade==1?-99:99;
      double t=((x-sx)*vx+(y-101)*80)/(99*99+80*80);
      double fade=t<.48?1:t<.8?1-(t-.48)/.32*.78:fmax(0,.22*(1-t)/.2);
      coverage*=fade;
    }
    double a=rgba[p*4+3]*coverage/255.;
    for(int k=0;k<3;k++)c->rgb[p*3+k]=(uint8_t)fmin(255,floor(rgba[p*4+k]*coverage+c->rgb[p*3+k]*(1-a)+.5));
  }
  return 0;
}
#ifdef H2_LUA_SOFTWARE_VECTORS
static char s_vector_coverage_key;
static int prepare_vector_coverage(lua_State *s) {
  lua_Integer payload=luaL_checkinteger(s,1);
  size_t bytes=payload>=0?h2_lua_vector_sw_cache_bytes((size_t)payload):0;
  if(!bytes)return luaL_error(s,"invalid vector coverage cache size");
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_coverage_key);
  if(!lua_isuserdata(s,-1)||lua_rawlen(s,-1)!=bytes) {
    lua_pop(s,1);lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_coverage_key);
    lua_gc(s,LUA_GCCOLLECT,0);
    void *storage=lua_newuserdatauv(s,bytes,0);
    if(!h2_lua_vector_sw_cache_init(storage,bytes))return luaL_error(s,"invalid vector coverage storage");
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_coverage_key);
  }
  lua_pop(s,1);lua_pushinteger(s,(lua_Integer)bytes);return 1;
}
#endif
/* Lua-authored geometric command streams use the same validated renderer. */
static int draw_vector_data(lua_State *s) {
  canvas_t *c=get_canvas(s);size_t length=0;
  const uint8_t *data=(const uint8_t *)luaL_checklstring(s,1,&length);double m[6];
  for(int i=0;i<6;i++)m[i]=finite_number(s,i+2);
  double opacity=lua_isnoneornil(s,8)?1:finite_number(s,8);
  if(opacity<0||opacity>1||fabs(m[0]*m[3]-m[1]*m[2])<1e-12)return luaL_error(s,"invalid vector transform");
  uint8_t *rgba=vector_scratch(s,(size_t)c->width*c->height*4);
#ifdef H2_LUA_SOFTWARE_VECTORS
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_coverage_key);
  h2_lua_vector_sw_cache_t *cache=lua_touserdata(s,-1);lua_pop(s,1);
  int rendered=render_sw_cached_output_for_lua(s,data,length,rgba,NULL,c->width,c->height,m,cache);
#else
  int rendered=h2_lua_vector_cg_render(data,length,rgba,c->width,c->height,m);
#endif
  if(!rendered)return luaL_error(s,"invalid Lua vector commands");
  if(opacity==1) {
    /* Exact integer source-over for byte premultiplied RGBA. An odd divisor
     * has no half-integer ties, so +127 preserves the double path's rounding. */
    for(size_t p=0;p<(size_t)c->width*c->height;p++)if(rgba[p*4+3]) {
      unsigned inverse=255u-rgba[p*4+3];
      for(unsigned k=0;k<3;k++) {
        unsigned value=rgba[p*4+k]+(c->rgb[p*3+k]*inverse+127u)/255u;
        c->rgb[p*3+k]=(uint8_t)(value>255u?255u:value);
      }
    }
    return 0;
  }
  for(size_t p=0;p<(size_t)c->width*c->height;p++)if(rgba[p*4+3]) {
    double a=rgba[p*4+3]*opacity/255.;
    for(int k=0;k<3;k++)c->rgb[p*3+k]=(uint8_t)fmin(255,floor(rgba[p*4+k]*opacity+c->rgb[p*3+k]*(1-a)+.5));
  }
  return 0;
}
typedef struct {
  const h2_lua_resource_t *resource;
  double focus,sheen;int dir,palette;
  uint8_t pixels[160*160*4];
} vector_icon_slot_t;
typedef struct { unsigned next;vector_icon_slot_t slots[6]; } vector_icon_cache_t;
static char s_vector_icon_cache_key;
/* Six transient rendered styles; keys include every visual parameter. */
/* Procedural focus, directional mask, tint and confirmation. No style atlas. */
static int draw_vector_icon(lua_State *s) {
  canvas_t *c=get_canvas(s);const h2_lua_resource_t *r=vector_resource(s);
  double focus=finite_number(s,2);int dir=(int)luaL_checkinteger(s,3),palette=(int)luaL_checkinteger(s,4);
  double sheen=finite_number(s,5),x=finite_number(s,6),y=finite_number(s,7),scale=finite_number(s,8),opacity=finite_number(s,9);
  if(focus<0||focus>1||dir<0||dir>2||palette< -1||palette>3||sheen< -2||sheen>1||scale<=0||scale>8||opacity<0||opacity>1)
    return luaL_error(s,"invalid vector icon style");
  const int w=160;size_t pixels=160u*160u;
  uint8_t *rgba=vector_scratch(s,pixels*(4+sizeof(float)*2));
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_vector_icon_cache_key);
  vector_icon_cache_t *cache=lua_touserdata(s,-1);
  if(!cache) {
    lua_pop(s,1);cache=lua_newuserdatauv(s,sizeof(*cache),0);memset(cache,0,sizeof(*cache));
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_vector_icon_cache_key);
  }
  lua_pop(s,1);
  for(unsigned i=0;i<6;i++) {
    vector_icon_slot_t *slot=&cache->slots[i];
    if(slot->resource==r&&slot->focus==focus&&slot->sheen==sheen&&slot->dir==dir&&slot->palette==palette) {
      rgba=slot->pixels;goto composite_icon;
    }
  }
  float *shadow=(float *)(rgba+pixels*4),*temp=shadow+pixels;
  double size=52+focus*36,left=(160-size)/2,m[6]={size/160,0,0,size/160,left,left};
  double geometry_key[6]={focus,(double)dir,0,0,0,0};
  vector_cache_t *geometry_cache=frame_cache(s);
  int geometry_hit=cached_pixels(geometry_cache,r,SIZE_MAX,0,w,w*2,geometry_key,rgba,NULL);
  if(geometry_hit<0)return luaL_error(s,"invalid cached icon geometry");
  double gain=focus>.5?1.05+focus*.32:1,alpha=.34+focus*.66;
  if(!geometry_hit) {
  if(!h2_lua_vector_cg_render(r->source,r->source_size,rgba,w,w,m))return luaL_error(s,"invalid vector icon");
  for(int yy=0;yy<w;yy++)for(int xx=0;xx<w;xx++) {
    size_t p=(size_t)yy*w+xx;double u=fmax(0,fmin(1,(xx+.5-left)/size)),mask=1;
    if(dir==1)mask=u<.42?u/.42*.32:.32+(u-.42)/.58*.68;
    if(dir==2)mask=u<.58?1-u/.58*.68:.32*(1-u)/.42;
    for(int k=0;k<4;k++)rgba[p*4+k]=(uint8_t)floor(rgba[p*4+k]*mask+.5);
    shadow[p]=rgba[p*4+3]/255.f;
  }
  if(focus>.5) {
    double sigma=(9+focus*8)/2,kernel[55],sum=0;int radius=(int)ceil(sigma*3);
    for(int k=-radius;k<=radius;k++){kernel[k+radius]=exp(-k*k/(2*sigma*sigma));sum+=kernel[k+radius];}
    for(int k=0;k<=radius*2;k++)kernel[k]/=sum;
    for(int yy=0;yy<w;yy++)for(int xx=0;xx<w;xx++) {
      double v=0;for(int k=-radius;k<=radius;k++)if(xx+k>=0&&xx+k<w)v+=shadow[yy*w+xx+k]*kernel[k+radius];
      temp[yy*w+xx]=(float)v;
    }
    for(int yy=0;yy<w;yy++)for(int xx=0;xx<w;xx++) {
      double v=0;for(int k=-radius;k<=radius;k++)if(yy+k>=0&&yy+k<w)v+=temp[(yy+k)*w+xx]*kernel[k+radius];
      shadow[yy*w+xx]=(float)(v*(.38+focus*.46));
    }
  } else memset(shadow,0,pixels*sizeof(float));
  /* Store the exact RGBA mask and float Gaussian shadow before tint/sheen.
   * The scratch layout is contiguous: RGBA followed by one float per pixel. */
  cache_pixels(geometry_cache,r,SIZE_MAX,0,w,w*2,geometry_key,rgba,NULL);
  }
  const double colors[4][3]={{.2,.82,1},{1,.53,.13},{.73,.3,1},{.2,1,.58}};
  for(size_t p=0;p<pixels;p++) {
    if(!rgba[p*4+3] && shadow[p]==0) {memset(rgba+p*4,0,4);continue;}
    float a=rgba[p*4+3]/255.f,alpha_f=(float)alpha;
    float outa=(a+shadow[p]*(1-a))*alpha_f;
    float rgb[3],highlight=0;
    for(unsigned k=0;k<3;k++) {
      rgb[k]=(canvas_minf(255*a,rgba[p*4+k]*(float)gain)+255*shadow[p]*(1-a))*alpha_f;
      highlight=canvas_maxf(highlight,outa?rgb[k]/outa/255.f:0);
    }
    float square=highlight*highlight;highlight=square*square*highlight*.4f;
    float stripe=sheen<0?0:canvas_maxf(0,1-fabsf(((p%160)+(p/160)*.3f)/160.f-(-.15f+(float)sheen*1.6f))/.09f);
    float output[4];int precise=0;
    for(unsigned k=0;k<3;k++) {
      float tint=palette<0?1:(float)colors[palette][k];
      float colored=rgb[k]*(tint+(1-tint)*highlight);
      float value=canvas_minf(255*outa,colored*(sheen>=-1?1.15f:1)+(255*outa-colored)*stripe*.85f);
      output[k]=floorf(value);
      /* Guard both sides of each truncation boundary; exact zero is safe.
       * A fully opaque source at alpha=1 has an exact 255 upper bound. */
      if(value>0 && !(value==255 && alpha==1 && rgba[p*4+3]==255))
        precise|=value-output[k]<.004f || output[k]+1-value<.004f;
    }
    float alpha_value=255*outa;
    output[3]=floorf(alpha_value+.5f);
    precise|=fabsf(alpha_value-(output[3]-.5f))<.004f;
    if(!precise) {
      for(unsigned k=0;k<4;k++)rgba[p*4+k]=(uint8_t)output[k];
      continue;
    }
    /* Preserve the original double/pow expression near byte boundaries. */
    {
    double a=rgba[p*4+3]/255.,outa=(a+shadow[p]*(1-a))*alpha;
    double rgb[3],highlight=0;
    for(int k=0;k<3;k++){rgb[k]=(fmin(255*a,rgba[p*4+k]*gain)+255*shadow[p]*(1-a))*alpha;highlight=fmax(highlight,outa?rgb[k]/outa/255:0);}
    highlight=pow(highlight,5)*.4;
    double stripe=sheen<0?0:fmax(0,1-fabs(((p%160)+(p/160)*.3)/160.-(-.15+sheen*1.6))/.09);
    for(int k=0;k<3;k++) {
      double colored=rgb[k]*(palette<0?1:colors[palette][k]+(1-colors[palette][k])*highlight);
      rgba[p*4+k]=(uint8_t)fmin(255*outa,colored*(sheen>=-1?1.15:1)+(255*outa-colored)*stripe*.85);
    }
    rgba[p*4+3]=(uint8_t)floor(255*outa+.5);
    }
  }
  vector_icon_slot_t *slot=&cache->slots[cache->next++%6];
  slot->resource=r;slot->focus=focus;slot->sheen=sheen;slot->dir=dir;slot->palette=palette;
  memcpy(slot->pixels,rgba,pixels*4);
composite_icon: ;
  int x0=(int)fmax(0,fmin(c->width,floor(x))),y0=(int)fmax(0,fmin(c->height,floor(y)));
  int x1=(int)fmax(0,fmin(c->width,ceil(x+160*scale))),y1=(int)fmax(0,fmin(c->height,ceil(y+160*scale)));
  /* Inverse coordinates are separable. Compute vertical coordinates once per row,
   * then use the FPU for filtering, retaining the original double calculation
   * at byte rounding boundaries. Transparent footprints need no blend. */
  for(int yy=y0;yy<y1;yy++) {
    double sy=fmax(0,fmin(159,(yy+.5-y)/scale-.5));
    int iy=(int)sy,iy1=iy<159?iy+1:iy;double v=sy-iy;
    for(int xx=x0;xx<x1;xx++) {
      double sx=fmax(0,fmin(159,(xx+.5-x)/scale-.5));int ix=(int)sx,ix1=ix<159?ix+1:ix;double u=sx-ix;
      size_t at[4]={(size_t)iy*160+ix,(size_t)iy*160+ix1,(size_t)iy1*160+ix,(size_t)iy1*160+ix1};
      if(!(rgba[at[0]*4+3]|rgba[at[1]*4+3]|rgba[at[2]*4+3]|rgba[at[3]*4+3]))continue;
      float uf=(float)u,vf=(float)v;
      float weights[4]={(1-uf)*(1-vf),uf*(1-vf),(1-uf)*vf,uf*vf},out[4]={0};
      for(int j=0;j<4;j++)for(int k=0;k<4;k++)out[k]+=rgba[at[j]*4+k]*weights[j];
      uint8_t *dest=c->rgb+((size_t)yy*c->width+xx)*3;float rounded[3];int precise=0;
      for(int k=0;k<3;k++) {
        float value=out[k]*(float)opacity+dest[k]*(1-out[3]*(float)opacity/255.f);
        rounded[k]=floorf(value+.5f);
        precise|=fabsf(value-(rounded[k]-.5f))<.002f;
      }
      if(precise) {
        double w[4]={(1-u)*(1-v),u*(1-v),(1-u)*v,u*v},exact[4]={0};
        for(int j=0;j<4;j++)for(int k=0;k<4;k++)exact[k]+=rgba[at[j]*4+k]*w[j];
        for(int k=0;k<3;k++)rounded[k]=(float)floor(exact[k]*opacity+dest[k]*(1-exact[3]*opacity/255)+.5);
      }
      for(int k=0;k<3;k++)dest[k]=(uint8_t)canvas_minf(255,rounded[k]);
    }
  }
  return 0;
}
/* Scene transitions occur outside composition. Keep the reusable primary
 * scratch allocation: repeatedly freeing a full-screen buffer lets long-lived
 * audio allocations fragment its space. Optional render caches and crossfade
 * slabs are released; authored resources and prepared bodies remain available. */
static int reset_vector_cache(lua_State *s) {
  lua_rawgetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  canvas_t *canvas = lua_touserdata(s, -1);
  lua_pop(s, 1);
  if (canvas && canvas->active)
    return luaL_error(s, "cannot reset vector cache during composition");
  const void *keys[] = {&s_vector_cache_key, &s_vector_icon_cache_key,
                         &s_vector_blend_scratch_key
#ifdef H2_LUA_SOFTWARE_VECTORS
                         ,&s_vector_rows_key
#endif
  };
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    lua_pushnil(s);
    lua_rawsetp(s, LUA_REGISTRYINDEX, keys[i]);
  }
  lua_gc(s, LUA_GCCOLLECT, 0);
  return 0;
}

#endif

static sprite_t *get_sprite(lua_State *s, h2_lua_job_t *job, const char *name) {
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_images_key);
  if(lua_isnil(s,-1)) {
    lua_pop(s,1);lua_newtable(s);lua_pushvalue(s,-1);
    lua_rawsetp(s,LUA_REGISTRYINDEX,&s_images_key);
  }
  lua_getfield(s,-1,name);
  sprite_t *sprite=lua_touserdata(s,-1);
  if(sprite) {lua_pop(s,2);return sprite;}
  lua_pop(s,1);
  const h2_lua_resource_t *resource=NULL;
  for(size_t i=0;i<job->host->config.resource_count;i++)
    if(strcmp(name,job->host->config.resources[i].name)==0)resource=&job->host->config.resources[i];
  if(!resource || resource->source_size<9 || memcmp(resource->source,"H2R8",4))
    luaL_error(s,"unknown RGBA sprite '%s'",name);
  const uint8_t *data=resource->source;
  unsigned w=data[4]|(unsigned)data[5]<<8u,h=data[6]|(unsigned)data[7]<<8u;
  if(!w || !h || w>4096 || h>4096)luaL_error(s,"invalid RGBA sprite dimensions");
  size_t bytes=(size_t)w*h*4u;
  size_t storage=bytes;
  unsigned mw=w,mh=h,levels=1;
  while(levels<13 && mw>1 && mh>1) {
    mw/=2;mh/=2;storage+=(size_t)mw*mh*4u;levels++;
  }
  sprite=lua_newuserdatauv(s,sizeof(*sprite)+storage,0);
  sprite->width=w;sprite->height=h;
  uLongf length=(uLongf)bytes;
  int result=uncompress(sprite->rgba,&length,data+8,(uLong)(resource->source_size-8));
  if(result!=Z_OK || length!=bytes)luaL_error(s,"corrupt RGBA sprite");
  for(size_t i=0;i<bytes;i+=4u)for(unsigned c=0;c<3;c++)
    sprite->rgba[i+c]=(uint8_t)(((unsigned)sprite->rgba[i+c]*sprite->rgba[i+3]+127u)/255u);
  sprite->levels=levels;sprite->level_width[0]=w;sprite->level_height[0]=h;sprite->level_offset[0]=0;
  for(unsigned level=1;level<levels;level++) {
    unsigned pw=sprite->level_width[level-1],ph=sprite->level_height[level-1];
    unsigned lw=pw/2,lh=ph/2;
    size_t offset=sprite->level_offset[level-1]+(size_t)pw*ph*4u;
    sprite->level_width[level]=lw;sprite->level_height[level]=lh;sprite->level_offset[level]=offset;
    const uint8_t *parent=sprite->rgba+sprite->level_offset[level-1];
    uint8_t *child=sprite->rgba+offset;
    for(unsigned y=0;y<lh;y++)for(unsigned x=0;x<lw;x++) {
      double left=(double)x*pw/lw,right=(double)(x+1)*pw/lw;
      double top=(double)y*ph/lh,bottom=(double)(y+1)*ph/lh,totals[4]={0};
      for(unsigned sy=(unsigned)floor(top);sy<(unsigned)ceil(bottom) && sy<ph;sy++)
      for(unsigned sx=(unsigned)floor(left);sx<(unsigned)ceil(right) && sx<pw;sx++) {
        double weight=(fmin(sx+1,right)-fmax(sx,left))*(fmin(sy+1,bottom)-fmax(sy,top));
        for(unsigned channel=0;channel<4;channel++)
          totals[channel]+=parent[((size_t)sy*pw+sx)*4u+channel]*weight;
      }
      double area=(right-left)*(bottom-top);
      for(unsigned channel=0;channel<4;channel++)
        child[((size_t)y*lw+x)*4u+channel]=(uint8_t)floor(totals[channel]/area+.5);
    }
  }
  lua_pushvalue(s,-1);lua_setfield(s,-3,name);lua_pop(s,2);
  return sprite;
}

/* Source pixel coordinates -> screen: x'=a*x+c*y+e, y'=b*x+d*y+f.
 * Sampling is bilinear in premultiplied RGBA, including source-over alpha. */
static int draw_affine_asset(lua_State *s) {
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  canvas_t *canvas=get_canvas(s);
  const char *name=luaL_checkstring(s,1);
  double a=finite_number(s,2),b=finite_number(s,3),c=finite_number(s,4),d=finite_number(s,5);
  double e=finite_number(s,6),f=finite_number(s,7),det=a*d-b*c;
  double opacity=lua_isnoneornil(s,9)?1:luaL_checknumber(s,9);
  if(!isfinite(opacity) || opacity<0 || opacity>1)return luaL_error(s,"invalid sprite opacity");
  if(fabs(det)<1e-12)return luaL_error(s,"singular sprite transform");
  sprite_t *sprite=get_sprite(s,job,name);
  double crop_x=0,crop_y=0,crop_w=sprite->width,crop_h=sprite->height;
  if(!lua_isnoneornil(s,8)) {
    luaL_checktype(s,8,LUA_TTABLE);double crop[4];
    for(int i=0;i<4;i++){lua_rawgeti(s,8,i+1);crop[i]=finite_number(s,-1);lua_pop(s,1);}
    crop_x=crop[0];crop_y=crop[1];crop_w=crop[2];crop_h=crop[3];
    if(crop_x<0 || crop_y<0 || crop_w<=0 || crop_h<=0 || crop_x+crop_w>sprite->width || crop_y+crop_h>sprite->height)
      return luaL_error(s,"invalid sprite crop");
  }
  /* Integer translated UI sprites need no inverse matrix or bilinear filter.
   * This is exactly the same source-over operation, including RGB rounding. */
  if(a==1 && b==0 && c==0 && d==1 && e==floor(e) && f==floor(f) &&
      crop_x==floor(crop_x) && crop_y==floor(crop_y) &&
      crop_w==floor(crop_w) && crop_h==floor(crop_h)) {
    int x0=(int)fmax(0,fmin(canvas->width,e+crop_x));
    int x1=(int)fmax(0,fmin(canvas->width,e+crop_x+crop_w));
    int y0=(int)fmax(0,fmin(canvas->height,f+crop_y));
    int y1=(int)fmax(0,fmin(canvas->height,f+crop_y+crop_h));
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++) {
      const uint8_t *src=sprite->rgba+((size_t)(y-(int)f)*sprite->width+x-(int)e)*4u;
      if(src[3]==0)continue;
      uint8_t *dst=canvas->rgb+((size_t)y*canvas->width+x)*3u;
      if(src[3]==255 && opacity==1){memcpy(dst,src,3);continue;}
      for(int channel=0;channel<3;channel++)
        dst[channel]=(uint8_t)fmin(255,floor(src[channel]*opacity+dst[channel]*(1-src[3]*opacity/255.)+.5));
    }
    return 0;
  }
  double scale=fmin(hypot(a,b),hypot(c,d));
  unsigned level=scale<1?(unsigned)fmax(0,floor(-log2(scale)+.5)):0;
  if(level>=sprite->levels)level=sprite->levels-1;
  unsigned sw=sprite->level_width[level],sh=sprite->level_height[level];
  const uint8_t *texels=sprite->rgba+sprite->level_offset[level];
  double minx=canvas->width,maxx=0,miny=canvas->height,maxy=0;
  for(int i=0;i<4;i++) {
    double sx=crop_x+((i&1)?crop_w:0),sy=crop_y+((i&2)?crop_h:0);
    double x=a*sx+c*sy+e,y=b*sx+d*sy+f;
    minx=fmin(minx,x);maxx=fmax(maxx,x);miny=fmin(miny,y);maxy=fmax(maxy,y);
  }
  int x0=(int)fmax(0,fmin(canvas->width,floor(minx))), x1=(int)fmax(0,fmin(canvas->width,ceil(maxx)));
  int y0=(int)fmax(0,fmin(canvas->height,floor(miny))), y1=(int)fmax(0,fmin(canvas->height,ceil(maxy)));
  for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++) {
    double px=x+.5-e,py=y+.5-f;
    double sx=(d*px-c*py)/det,sy=(-b*px+a*py)/det;
    if(sx<crop_x || sy<crop_y || sx>=crop_x+crop_w || sy>=crop_y+crop_h)continue;
    sx=fmax(0,fmin(sw-1,sx*sw/sprite->width-.5));sy=fmax(0,fmin(sh-1,sy*sh/sprite->height-.5));
    unsigned ix=(unsigned)sx,iy=(unsigned)sy,ix1=ix+1<sw?ix+1:ix,iy1=iy+1<sh?iy+1:iy;
    double ux=sx-ix,uy=sy-iy,rgba[4]={0};
    const uint8_t *samples[]={texels+((size_t)iy*sw+ix)*4u,
      texels+((size_t)iy*sw+ix1)*4u,texels+((size_t)iy1*sw+ix)*4u,texels+((size_t)iy1*sw+ix1)*4u};
    double weights[]={(1-ux)*(1-uy),ux*(1-uy),(1-ux)*uy,ux*uy};
    for(int k=0;k<4;k++)for(int channel=0;channel<4;channel++)rgba[channel]+=samples[k][channel]*weights[k];
    uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3u;
    for(int channel=0;channel<3;channel++) {
      double value=rgba[channel]*opacity+pixel[channel]*(1-rgba[3]*opacity/255.0);
      pixel[channel]=(uint8_t)fmin(255,floor(value+.5));
    }
  }
  return 0;
}

static unsigned style_u16(const uint8_t *p) {return p[0]|(unsigned)p[1]<<8;}
static uint32_t style_u32(const uint8_t *p) {
  return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}


typedef struct style_cache {
  const h2_lua_resource_t *resource;
  unsigned width,height,next,index[6];
  unsigned minx[6],miny[6],maxx[6],maxy[6];
  uint8_t pixels[];
} style_cache_t;

/* Six bounded cached styles, not a decoded cache of every focus variant. The two styles
 * are interpolated in premultiplied RGBA before a single source-over operation. */
static int draw_sprite_atlas(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
  h2_lua_job_t *job=lua_touserdata(s,lua_upvalueindex(1));
  const char *name=luaL_checkstring(s,1);
  lua_Integer indices[]={luaL_checkinteger(s,2),luaL_checkinteger(s,3)};
  double blend=finite_number(s,4),dx=finite_number(s,5),dy=finite_number(s,6);
  double scale=lua_isnoneornil(s,7)?1:finite_number(s,7);
  double opacity=lua_isnoneornil(s,8)?1:finite_number(s,8);
  if(scale<=0 || scale>8)return luaL_error(s,"invalid sprite scale");
  if(opacity<0 || opacity>1)return luaL_error(s,"invalid sprite opacity");
  if(blend<0 || blend>1)return luaL_error(s,"invalid sprite blend");
  const h2_lua_resource_t *res=NULL;
  for(size_t i=0;i<job->host->config.resource_count;i++)
    if(strcmp(name,job->host->config.resources[i].name)==0)res=&job->host->config.resources[i];
  if(!res || res->source_size<12 || memcmp(res->source,"H2RS",4))return luaL_error(s,"unknown sprite atlas");
  const uint8_t *data=res->source;
  unsigned w=style_u16(data+4),h=style_u16(data+6),count=style_u16(data+8);
  if(!w || !h || w>256 || h>256 || !count || count>512 || (style_u16(data+10) ||
      res->source_size<12u+count*8u))return luaL_error(s,"invalid sprite atlas header");
  size_t bytes=(size_t)w*h*4u;
  lua_rawgetp(s,LUA_REGISTRYINDEX,&s_styles_key);
  style_cache_t *cache=lua_touserdata(s,-1);
  if(!cache || lua_rawlen(s,-1)<sizeof(*cache)+bytes*6) {
    /* Replacing the registry value alone leaves the old userdata alive until
     * the next GC cycle.  When a scene switches from a smaller atlas to a
     * larger one, allocating both caches at once can exceed an otherwise
     * sufficient VM budget.  Drop the sole strong reference and collect it
     * before allocating the replacement so cache growth is not additive. */
    int replacing=cache!=NULL;
    lua_pop(s,1);
    if(replacing) {
      lua_pushnil(s);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_styles_key);
      lua_gc(s,LUA_GCCOLLECT,0);
    }
    cache=lua_newuserdatauv(s,sizeof(*cache)+bytes*6,0);
    memset(cache,0,sizeof(*cache));
    lua_pushvalue(s,-1);lua_rawsetp(s,LUA_REGISTRYINDEX,&s_styles_key);
  }
  if(cache->resource!=res || cache->width!=w || cache->height!=h) {
    memset(cache,0,sizeof(*cache));cache->resource=res;cache->width=w;cache->height=h;
  }
  const uint8_t *samples[2];unsigned slots[2];
  for(int k=0;k<2;k++) {
    if(indices[k]<1 || indices[k]>count)return luaL_error(s,"invalid sprite atlas index");
    const uint8_t *entry=data+12+(size_t)(indices[k]-1)*8;
    size_t offset=style_u32(entry),length=style_u32(entry+4);
    if(offset<12u+count*8u || offset>res->source_size || !length || length>res->source_size-offset)
      return luaL_error(s,"invalid sprite atlas range");
    unsigned slot=0;
    for(;slot<6;slot++)if(cache->index[slot]==(unsigned)indices[k])break;
    if(slot<6){samples[k]=cache->pixels+slot*bytes;slots[k]=slot;continue;}
    slot=cache->next++%6;
    if(k && slot==slots[0])slot=cache->next++%6;
    cache->index[slot]=0;
    uint8_t *pixels=cache->pixels+slot*bytes;
    uLongf decoded=(uLongf)bytes;
    if(uncompress(pixels,&decoded,data+offset,(uLong)length)!=Z_OK || decoded!=bytes)
      return luaL_error(s,"corrupt sprite atlas");
    for(size_t i=0;i<bytes;i+=4)for(int c=0;c<3;c++)pixels[i+c]=(uint8_t)((pixels[i+c]*pixels[i+3]+127u)/255u);
    cache->minx[slot]=w;cache->miny[slot]=h;cache->maxx[slot]=cache->maxy[slot]=0;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)if(pixels[((size_t)y*w+x)*4+3]) {
      if(x<cache->minx[slot])cache->minx[slot]=x;
      if(y<cache->miny[slot])cache->miny[slot]=y;
      if(x+1>cache->maxx[slot])cache->maxx[slot]=x+1;
      if(y+1>cache->maxy[slot])cache->maxy[slot]=y+1;
    }
    cache->index[slot]=(unsigned)indices[k];samples[k]=pixels;slots[k]=slot;
  }
  double minx=fmin(cache->minx[slots[0]],cache->minx[slots[1]]),maxx=fmax(cache->maxx[slots[0]],cache->maxx[slots[1]]);
  double miny=fmin(cache->miny[slots[0]],cache->miny[slots[1]]),maxy=fmax(cache->maxy[slots[0]],cache->maxy[slots[1]]);
  int x0=(int)fmax(0,fmin(canvas->width,floor(dx+minx*scale))),x1=(int)fmax(0,fmin(canvas->width,ceil(dx+maxx*scale)));
  int y0=(int)fmax(0,fmin(canvas->height,floor(dy+miny*scale))),y1=(int)fmax(0,fmin(canvas->height,ceil(dy+maxy*scale)));
  for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++) {
    double sx=scale==1?x-dx:(x+.5-dx)/scale-.5,sy=scale==1?y-dy:(y+.5-dy)/scale-.5;
    int ix=(int)floor(sx),iy=(int)floor(sy);
    double u=sx-ix,v=sy-iy,rgba[4]={0};
    for(int yy=0;yy<2;yy++)for(int xx=0;xx<2;xx++) {
      int tx=ix+xx,ty=iy+yy;if(tx<0 || ty<0 || tx>=(int)w || ty>=(int)h)continue;
      double weight=(xx?u:1-u)*(yy?v:1-v);
      size_t p=((size_t)ty*w+tx)*4;
      for(int c=0;c<4;c++)rgba[c]+=weight*(samples[0][p+c]*(1-blend)+samples[1][p+c]*blend);
    }
    uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3;
    for(int c=0;c<3;c++)pixel[c]=(uint8_t)fmin(255,floor(rgba[c]*opacity+pixel[c]*(1-rgba[3]/255.*opacity)+.5));
  }
  lua_pop(s,1);
  return 0;
}

void h2_lua_canvas_register(lua_State *s,h2_lua_job_t *job) {
  const struct {const char *name;lua_CFunction fn;} functions[]={
    {"begin_composite",begin_composite},{"end_composite",end_composite},{"fade_composite",fade_composite},
    {"add_disc",add_disc},{"add_line",add_line},{"add_lines",add_lines},{"draw_affine_asset",draw_affine_asset},
    {"draw_polygon",draw_polygon},{"over_line",over_line},
    {"glow_line",glow_line},
    {"draw_sprite_atlas",draw_sprite_atlas},
#ifdef H2_QI_DUEL_DESKTOP_VECTORS
    {"prepare_vector",prepare_vector},{"reset_vector_cache",reset_vector_cache},{"draw_vector_slice",draw_vector_slice},{"draw_vector_data",draw_vector_data},{"draw_vector_affine",draw_vector_affine},{"draw_vector_icon",draw_vector_icon},
#ifdef H2_LUA_SOFTWARE_VECTORS
    {"prepare_vector_coverage",prepare_vector_coverage},
#endif
#endif
  };
  for(size_t i=0;i<sizeof(functions)/sizeof(functions[0]);i++) {
    lua_pushlightuserdata(s,job);lua_pushcclosure(s,functions[i].fn,1);
    lua_setfield(s,-2,functions[i].name);
  }
}
