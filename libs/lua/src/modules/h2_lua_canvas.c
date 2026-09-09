#include "h2_lua_canvas.h"

#include <math.h>
#include <string.h>
#include "zlib.h"

typedef struct canvas {
  int width, height, active;
  uint8_t rgb[];
} canvas_t;
typedef struct sprite {
  unsigned width, height;
  unsigned levels, level_width[13], level_height[13];
  size_t level_offset[13];
  uint8_t rgba[]; /* premultiplied for filtering */
} sprite_t;
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

static canvas_t *get_canvas(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  lua_rawgetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  canvas_t *canvas = lua_touserdata(s, -1);
  lua_pop(s, 1);
  if (!job->display_open || !canvas || !canvas->active ||
      canvas->width != job->display_info.width || canvas->height != job->display_info.height)
    luaL_error(s, "canvas composition is not open");
  return canvas;
}

static int begin_composite(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  if (!job->display_open) return luaL_error(s, "display is not open");
  unsigned w = (unsigned)job->display_info.width, h = (unsigned)job->display_info.height;
  if (!w || !h || w > 4096u || h > 4096u) return luaL_error(s, "invalid canvas size");
  lua_rawgetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  canvas_t *canvas = lua_touserdata(s, -1);
  if (canvas && canvas->active) return luaL_error(s, "canvas composition already open");
  size_t bytes = sizeof(canvas_t) + (size_t)w * h * 3u;
  if (!canvas || lua_rawlen(s, -1) < bytes) {
    lua_pop(s, 1);
    lua_pushnil(s); lua_rawsetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
    canvas = lua_newuserdatauv(s, bytes, 0);
    lua_pushvalue(s, -1); lua_rawsetp(s, LUA_REGISTRYINDEX, &s_canvas_key);
  }
  canvas->width = (int)w; canvas->height = (int)h; canvas->active = 1;
  for (size_t i = 0; i < (size_t)w * h; ++i) {
    uint16_t color = job->framebuffer[i];
    unsigned r = color >> 11u, g = (color >> 5u) & 63u, b = color & 31u;
    canvas->rgb[i*3u] = (uint8_t)((r<<3u)|(r>>2u));
    canvas->rgb[i*3u+1u] = (uint8_t)((g<<2u)|(g>>4u));
    canvas->rgb[i*3u+2u] = (uint8_t)((b<<3u)|(b>>2u));
  }
  lua_pop(s, 1);
  return 0;
}

static int end_composite(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  canvas_t *canvas = get_canvas(s);
  for (size_t i = 0; i < (size_t)canvas->width * canvas->height; ++i)
    job->framebuffer[i] = (uint16_t)((canvas->rgb[i*3u]>>3u)<<11u |
        (canvas->rgb[i*3u+1u]>>2u)<<5u | (canvas->rgb[i*3u+2u]>>3u));
  canvas->active = 0;
  job->dirty_valid = 1; job->dirty_min_x = 0; job->dirty_min_y = 0;
  job->dirty_max_x = canvas->width-1; job->dirty_max_y = canvas->height-1;
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

static double distance2(double x, double y, double ax, double ay,
                         double dx, double dy, double inverse_length2) {
  double t = ((x-ax)*dx + (y-ay)*dy)*inverse_length2;
  t = fmax(0,fmin(1,t));
  double px=x-ax-t*dx, py=y-ay-t*dy;
  return px*px+py*py;
}

/* Small additive round-capped strokes, with fractional endpoints and width.
 * Interior/exterior pixels take a fast path; only boundary coverage is sampled.
 * They deliberately compose in RGB888 so overlapping trail segments preserve
 * the reference's additive joint highlights before one RGB565 conversion. */
static void add_capsule(canvas_t *canvas, double ax, double ay, double bx, double by,
                         double radius, const double rgb[3], double over_alpha) {
  if (radius <= 0 || (rgb[0]==0 && rgb[1]==0 && rgb[2]==0)) return;
  int x0=(int)fmax(0,fmin(canvas->width,floor(fmin(ax,bx)-radius)));
  int x1=(int)fmax(0,fmin(canvas->width,ceil(fmax(ax,bx)+radius)));
  int y0=(int)fmax(0,fmin(canvas->height,floor(fmin(ay,by)-radius)));
  int y1=(int)fmax(0,fmin(canvas->height,ceil(fmax(ay,by)+radius)));
  double dx=bx-ax,dy=by-ay, length2=dx*dx+dy*dy;
  double inverse=length2>0 ? 1/length2 : 0, rr=radius*radius;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) {
    double dist=distance2(x+.5,y+.5,ax,ay,dx,dy,inverse);
    if(dist >= (radius+.708)*(radius+.708)) continue;
    double coverage=1;
    if(radius < .708 || dist > (radius-.708)*(radius-.708)) {
      unsigned hit=0;
      for(unsigned sy=0;sy<8;sy++)for(unsigned sx=0;sx<8;sx++)
        hit += distance2(x+(sx+.5)/8,y+(sy+.5)/8,ax,ay,dx,dy,inverse)<=rr;
      coverage=hit/64.0;
    }
    uint8_t *pixel=canvas->rgb+((size_t)y*canvas->width+x)*3u;
    for(int c=0;c<3;c++) {
      unsigned value=(unsigned)floor(pixel[c]*(over_alpha<0?1:1-over_alpha*coverage)+rgb[c]*coverage+.5);
      pixel[c]=(uint8_t)(value>255u?255u:value);
    }
  }
}

static int add_disc(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
  double x=finite_number(s,1),y=finite_number(s,2),r=finite_number(s,3),rgb[3];
  if(r<0 || r>64) return luaL_error(s,"invalid canvas disc radius");
  color_args(s,4,5,rgb);
  add_capsule(canvas,x,y,x,y,r,rgb,-1);
  return 0;
}

static int add_line(lua_State *s) {
  canvas_t *canvas=get_canvas(s);
  double ax=finite_number(s,1),ay=finite_number(s,2),bx=finite_number(s,3),by=finite_number(s,4);
  double width=finite_number(s,5),rgb[3];
  if(width<0 || width>128) return luaL_error(s,"invalid canvas line width");
  color_args(s,6,7,rgb);
  add_capsule(canvas,ax,ay,bx,by,width*.5,rgb,-1);
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
    for(int y=firsty;y<lasty;y++)for(int x=firstx;x<lastx;x++) {
      unsigned hits=0;
      for(int sy=0;sy<8;sy++)for(int sx=0;sx<8;sx++) {
        double px=x+x0+(sx+.5)/8,py=y+y0+(sy+.5)/8,dist=1e30;int inside=0;
        for(size_t i=0,j=count-1;i<count;j=i++) {
          double ax=points[j][0],ay=points[j][1],bx=points[i][0],by=points[i][1];
          if((ay>py)!=(by>py) && px<(bx-ax)*(py-ay)/(by-ay)+ax)inside=!inside;
          if(pass){double dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;
            dist=fmin(dist,distance2(px,py,ax,ay,dx,dy,len>0?1/len:0));}
        }
        hits+=pass?(dist<=width*width*.25):inside;
      }
      mask[(size_t)y*w+x]=hits/64.f;
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
      double value=0;for(int n=-radius;n<=radius;n++)if(x+n>=0 && x+n<w)value+=mask[(size_t)y*w+x+n]*kernel[n+radius];
      tmp[(size_t)y*w+x]=(float)value;
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
      double value=0;for(int n=-radius;n<=radius;n++)if(y+n>=0 && y+n<h)value+=tmp[(size_t)(y+n)*w+x]*kernel[n+radius];
      soft[(size_t)y*w+x]=(float)value;
    }
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
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
  if(!w || !h || w>256 || h>256 || !count || count>512 || style_u16(data+10) ||
      res->source_size<12u+count*8u)return luaL_error(s,"invalid sprite atlas header");
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
    {"begin_composite",begin_composite},{"end_composite",end_composite},
    {"add_disc",add_disc},{"add_line",add_line},{"draw_affine_asset",draw_affine_asset},
    {"draw_polygon",draw_polygon},{"over_line",over_line},
    {"glow_line",glow_line},
    {"draw_sprite_atlas",draw_sprite_atlas},
  };
  for(size_t i=0;i<sizeof(functions)/sizeof(functions[0]);i++) {
    lua_pushlightuserdata(s,job);lua_pushcclosure(s,functions[i].fn,1);
    lua_setfield(s,-2,functions[i].name);
  }
}
