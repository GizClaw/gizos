#include "h2_lua_fishing_math.h"
#include "lua.h"
#include "lauxlib.h"
#include "h2_runtime.h"
#include "h2_lua_fpu_math.h"
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAX_NODES 256
/* Two-float coordinates keep sub-float residuals during constraint sweeps.
 * Only this scratch representation uses them; Lua/world state remains double.
 * Local integration displacements can use float. Explicit fmaf preserves the
 * product roundoff on the S3 FPU. */
typedef struct { float hi,lo; } precise_float;
#if defined(_MSC_VER)
#define FISHING_INLINE __forceinline
#else
#define FISHING_INLINE inline __attribute__((always_inline))
#endif
static FISHING_INLINE precise_float pf_from(double x) {
  float hi=(float)x;return (precise_float){hi,(float)(x-(double)hi)};
}
static FISHING_INLINE precise_float pf_add(precise_float a,precise_float b) {
  float sum=a.hi+b.hi,v=sum-a.hi;
  float error=(a.hi-(sum-v))+(b.hi-v)+a.lo+b.lo;
  float hi=sum+error;return (precise_float){hi,error-(hi-sum)};
}
static FISHING_INLINE precise_float pf_sub(precise_float a,precise_float b) {
  return pf_add(a,(precise_float){-b.hi,-b.lo});
}
static FISHING_INLINE precise_float pf_square(precise_float a) {
  float product=a.hi*a.hi,error=fmaf(a.hi,a.hi,-product)+2.0f*a.hi*a.lo+a.lo*a.lo;
  float hi=product+error;return (precise_float){hi,error-(hi-product)};
}
/* Float square-root seed followed by a double Newton refinement. World
 * state remains double; for normal game magnitudes the seed's relative
 * error is squared (~1e-14). Extreme values retain the libm reference path. */
static double refined_sqrt(double value) {
  if(value<1e-20 || value>1e20) return sqrt(value);
  double root=(double)sqrtf((float)value);
  return .5*(root+value/root);
}
static double number(lua_State *s, int at) {
  double v=luaL_checknumber(s,at);
  if (!isfinite(v) || fabs(v)>1e8) luaL_error(s,"invalid rope value");
  return v;
}
static double field(lua_State *s,const char *name) {
  lua_getfield(s,1,name);double v=number(s,-1);lua_pop(s,1);return v;
}
static int flag(lua_State *s,const char *name) {
  lua_getfield(s,1,name);int v=lua_toboolean(s,-1);lua_pop(s,1);return v;
}
/* Exact alternating XPBD tension-only constraint pass used by Lua fishing.
 * Integration, payout, water drag and fish behavior stay with the Lua owner. */
static int solve_rope(lua_State *s) {
  double p[MAX_NODES][3],rest[MAX_NODES],lambda[MAX_NODES]={0};
  luaL_checktype(s,1,LUA_TTABLE);
  double h=number(s,2);int iterations=(int)luaL_checkinteger(s,3);
  if (h<1e-6 || h>.1 || iterations<1 || iterations>32) return luaL_error(s,"invalid rope step");
  double mass=field(s,"mass"),density=field(s,"density"),axial=field(s,"axial"),paid=field(s,"paid");
  if (mass<1e-9 || density<1e-9 || axial<1e-9 || paid<1e-9) return luaL_error(s,"invalid rope material");
  int landed=flag(s,"landed"),surface=flag(s,"surface"),retrieving=flag(s,"retrieving_started");
  lua_getfield(s,1,"p");luaL_checktype(s,-1,LUA_TTABLE);int positions=lua_gettop(s);
  size_t n=lua_rawlen(s,positions);
  if (n<2 || n>MAX_NODES) return luaL_error(s,"invalid rope node count");
  for(size_t i=0;i<n;++i) {
    lua_rawgeti(s,positions,(lua_Integer)i+1);luaL_checktype(s,-1,LUA_TTABLE);
    for(int j=0;j<3;++j) {lua_rawgeti(s,-1,j+1);p[i][j]=number(s,-1);lua_pop(s,1);}
    lua_pop(s,1);
  }
  lua_getfield(s,1,"rest");luaL_checktype(s,-1,LUA_TTABLE);
  if(lua_rawlen(s,-1)!=n-1) return luaL_error(s,"invalid rope spans");
  for(size_t i=0;i+1<n;++i) {
    lua_rawgeti(s,-1,(lua_Integer)i+1);rest[i]=number(s,-1);lua_pop(s,1);
    if(rest[i]<1e-9) return luaL_error(s,"invalid rope rest length");
  }
  lua_pop(s,1);
  lua_getfield(s,1,"lambda");luaL_checktype(s,-1,LUA_TTABLE);
  int lambdas=lua_gettop(s);
  double wa_cache[MAX_NODES],wb_cache[MAX_NODES],alpha_cache[MAX_NODES];
  for(size_t i=0;i+1<n;++i) {
    double node_mass=fmax(.00002,density*rest[i]);
    wa_cache[i]=i==0?0:1/node_mass;wb_cache[i]=i+2==n?1/mass:1/node_mass;
    alpha_cache[i]=(rest[i]/axial)/(h*h);
  }
  double span_lambda=0;
  for(int iteration=0;iteration<iterations;++iteration) {
    for(size_t k=0;k+1<n;++k) {
      size_t i=iteration%2==0?k:n-2-k;
      double *a=p[i],*b=p[i+1];double x=b[0]-a[0],y=b[1]-a[1],z=b[2]-a[2];
      double squared=x*x+y*y+z*z;
      /* A slack, zero-lambda tension-only span makes no correction. Keep a
       * margin near the boundary so the original double path decides it. */
      if(lambda[i]==0 && squared<rest[i]*rest[i]*(1-1e-12)) continue;
      double d=refined_sqrt(squared);if(d<1e-8) continue;
      double wa=wa_cache[i],wb=wb_cache[i],alpha=alpha_cache[i],old=lambda[i];
      double next=fmin(0,old+(-(d-rest[i])-alpha*old)/(wa+wb+alpha));
      double dl=next-old;lambda[i]=next;double q=dl/d;
      if(wa>0) {a[0]-=wa*q*x;a[1]-=wa*q*y;a[2]-=wa*q*z;}
      if(wb>0) {b[0]+=wb*q*x;b[1]+=wb*q*y;b[2]+=wb*q*z;}
    }
    if(n>2) {
      double *end=p[n-1];double x=end[0]-p[0][0],y=end[1]-p[0][1],z=end[2]-p[0][2];
      double distance=refined_sqrt(x*x+y*y+z*z);
      if(distance>paid) {
        double w=1/mass,alpha=paid/axial/(h*h);
        double dl=(-(distance-paid)-alpha*span_lambda)/(w+alpha);span_lambda+=dl;
        double q=w*dl/distance;end[0]+=q*x;end[1]+=q*y;end[2]+=q*z;
      }
    }
    if(!landed) {for(size_t i=1;i+1<n;++i) if(p[i][1]<0) p[i][1]=0;}
    else if(!retrieving) p[n-1][1]=surface?0:fmin(0,p[n-1][1]);
  }
  for(size_t i=0;i<n;++i) {
    lua_rawgeti(s,positions,(lua_Integer)i+1);
    for(int j=0;j<3;++j) {lua_pushnumber(s,p[i][j]);lua_rawseti(s,-2,j+1);}
    lua_pop(s,1);
  }
  for(size_t i=0;i+1<n;++i) {lua_pushnumber(s,lambda[i]);lua_rawseti(s,lambdas,(lua_Integer)i+1);}
  lua_pushnumber(s,span_lambda);return 1;
}
typedef struct rod_geometry {
  double base[65][3], segment[65][5], root[3], pivot[3];
  double length,onset;
  double moment[18][3],parallel[3];
} rod_geometry_t;
#define ROD_META "fishing.elastic_rod"
static double optional_field(lua_State *s,const char *name,double fallback) {
  lua_getfield(s,1,name);double v=lua_isnil(s,-1)?fallback:number(s,-1);lua_pop(s,1);return v;
}
static double norm3(double x,double y,double z) {return refined_sqrt(x*x+y*y+z*z);}
static void push_point(lua_State *s,const double *p,int n) {
  lua_createtable(s,n,0);for(int i=0;i<n;++i){lua_pushnumber(s,p[i]);lua_rawseti(s,-2,i+1);}
}
static int cached_points(lua_State *s,int geometry,int slot,int count,int dimensions) {
  lua_getiuservalue(s,geometry,slot);
  if(!lua_istable(s,-1)) {
    lua_pop(s,1);lua_createtable(s,count,0);double zero[3]={0};
    for(int i=1;i<=count;++i){push_point(s,zero,dimensions);lua_rawseti(s,-2,i);}
    lua_pushvalue(s,-1);lua_setiuservalue(s,geometry,slot);
  }
  return lua_gettop(s);
}
static void update_point(lua_State *s,int table,int at,const double *p,int dimensions) {
  lua_rawgeti(s,table,at);
  for(int j=0;j<dimensions;++j){lua_pushnumber(s,p[j]);lua_rawseti(s,-2,j+1);}lua_pop(s,1);
}
/* The same calibrated rod, oblique pivot and boat-rail transform as world_rod.
 * Only immutable geometry is cached; all 64 elastic segments remain present. */
static int rod_pose(lua_State *s) {
  luaL_checktype(s,1,LUA_TTABLE);int tip_only=lua_toboolean(s,2);
  double onset=number(s,3),length=field(s,"length_m");
  if(onset<0 || onset>=1 || length<=0 || length>30) return luaL_error(s,"invalid rod geometry");
  lua_getfield(s,1,"native_rod");rod_geometry_t *g=luaL_testudata(s,-1,ROD_META);
  if(!g || g->length!=length || g->onset!=onset) {
    lua_pop(s,1);g=lua_newuserdatauv(s,sizeof(*g),3);g->length=length;g->onset=onset;
    lua_getfield(s,1,"world_base");luaL_checktype(s,-1,LUA_TTABLE);
    if(lua_rawlen(s,-1)!=65) return luaL_error(s,"invalid reference rod");
    for(int i=0;i<65;++i) {
      lua_rawgeti(s,-1,i+1);luaL_checktype(s,-1,LUA_TTABLE);
      for(int j=0;j<3;++j){lua_rawgeti(s,-1,j+1);g->base[i][j]=number(s,-1);lua_pop(s,1);}lua_pop(s,1);
    }
    lua_pop(s,1);
    double d=norm3(g->base[1][0]-g->base[0][0],g->base[1][1]-g->base[0][1],g->base[1][2]-g->base[0][2]);
    if(d<1e-9) return luaL_error(s,"zero rod grip segment");
    for(int j=0;j<3;++j){g->root[j]=(g->base[1][j]-g->base[0][j])*.25/d;g->pivot[j]=g->base[0][j]-g->root[j];}
    double scale=length/2.03;
    memset(g->moment,0,sizeof(g->moment));memset(g->parallel,0,sizeof(g->parallel));
    for(int i=1;i<65;++i) {
      for(int j=0;j<3;++j) g->segment[i][j]=(g->base[i][j]-g->base[i-1][j])*scale;
      g->segment[i][3]=pow(fmax(0,((double)i/64-onset)/(1-onset)),1.5);
      g->segment[i][4]=.75*g->segment[i][0]+.5*g->segment[i][1]+.4330127*g->segment[i][2];
      const double axis[]={.75,.5,.4330127};
      for(int j=0;j<3;++j) {
        double along=axis[j]*g->segment[i][4],v=g->segment[i][j]-along,power=1;
        g->parallel[j]+=along;
        for(int k=0;k<18;++k){g->moment[k][j]+=v*power;power*=g->segment[i][3];}
      }
    }
    luaL_newmetatable(s,ROD_META);lua_setmetatable(s,-2);lua_pushvalue(s,-1);lua_setfield(s,1,"native_rod");
  }
  int geometry=lua_gettop(s);
  double angle=-(field(s,"angle")+1.98),q=field(s,"q"),yaw=optional_field(s,"yaw",0);
  double rail=optional_field(s,"rail_blend",0),f[3]={0};int fighting=0;
  lua_getfield(s,1,"fight_dir");
  if(!lua_isnil(s,-1)) {luaL_checktype(s,-1,LUA_TTABLE);fighting=1;
    for(int i=0;i<3;++i){lua_rawgeti(s,-1,i+1);f[i]=number(s,-1);lua_pop(s,1);}}
  lua_pop(s,1);
  double points[65][3],ca=cos(angle),sa=sin(angle),cy=cos(yaw),sy=sin(yaw);
  double rotation[3][3];
  if(fighting) {
    /* The unbent fight rotation is shared by all 64 segments. Compose yaw
     * once, rather than repeating Rodrigues/yaw products for every segment. */
    const double axis[3]={.75,.5,.4330127};
    for(int j=0;j<3;++j) {
      double v[3]={0,0,0};v[j]=1;
      double cross[3]={axis[1]*v[2]-axis[2]*v[1],axis[2]*v[0]-axis[0]*v[2],axis[0]*v[1]-axis[1]*v[0]};
      double r[3];for(int k=0;k<3;++k)r[k]=v[k]*ca+cross[k]*sa+axis[k]*axis[j]*(1-ca);
      rotation[0][j]=r[0]*cy+r[2]*sy;rotation[1][j]=r[1];rotation[2][j]=-r[0]*sy+r[2]*cy;
    }
  }
  double x=g->root[0],y=g->root[1],z=g->root[2],dot=.75*x+.5*y+.4330127*z;
  points[0][0]=g->pivot[0]+x*ca+(.5*z-.4330127*y)*sa+.75*dot*(1-ca);
  points[0][1]=g->pivot[1]+y*ca+(.4330127*x-.75*z)*sa+.5*dot*(1-ca);
  points[0][2]=g->pivot[2]+z*ca+(.75*y-.5*x)*sa+.4330127*dot*(1-ca);
  double last[3]={points[0][0],points[0][1],points[0][2]};
  if(tip_only && !fighting && fabs(q)<=1.6) {
    /* Sum all 64 bending segments using cached shape moments. Taylor degree
     * 17 at |q|<=1.6 has <1e-9 m remainder at supported rod lengths. */
    double cv[3]={0},sv[3]={0},power=1;
    for(int k=0;k<18;++k) {
      for(int j=0;j<3;++j) {
        if(k%2==0)cv[j]+=power*g->moment[k][j];else sv[j]+=power*g->moment[k][j];
      }
      power*=q/(k+1);
      if(k%2==1)power=-power;
    }
    double cross_c[]={.5*cv[2]-.4330127*cv[1],.4330127*cv[0]-.75*cv[2],.75*cv[1]-.5*cv[0]};
    double cross_s[]={.5*sv[2]-.4330127*sv[1],.4330127*sv[0]-.75*sv[2],.75*sv[1]-.5*sv[0]};
    double dx=g->parallel[0]+ca*cv[0]+sa*sv[0]+sa*cross_c[0]-ca*cross_s[0];
    double dy=g->parallel[1]+ca*cv[1]+sa*sv[1]+sa*cross_c[1]-ca*cross_s[1];
    double dz=g->parallel[2]+ca*cv[2]+sa*sv[2]+sa*cross_c[2]-ca*cross_s[2];
    if(yaw!=0){double xx=dx*cy+dz*sy;dz=-dx*sy+dz*cy;dx=xx;}
    last[0]+=dx;last[1]+=dy;last[2]+=dz;
  } else for(int i=1;i<65;++i) {
    double shape=g->segment[i][3],a=angle-(fighting?0:q)*shape;
    double c=(fighting || shape==0)?ca:cos(a),ss=(fighting || shape==0)?sa:sin(a);
    x=g->segment[i][0];y=g->segment[i][1];z=g->segment[i][2];dot=g->segment[i][4];
    double dx,dy,dz;
    if(fighting) {
      dx=rotation[0][0]*x+rotation[0][1]*y+rotation[0][2]*z;
      dy=rotation[1][0]*x+rotation[1][1]*y+rotation[1][2]*z;
      dz=rotation[2][0]*x+rotation[2][1]*y+rotation[2][2]*z;
    } else {
      dx=x*c+(.5*z-.4330127*y)*ss+.75*dot*(1-c);
      dy=y*c+(.4330127*x-.75*z)*ss+.5*dot*(1-c);
      dz=z*c+(.75*y-.5*x)*ss+.4330127*dot*(1-c);
      if(yaw!=0){double xx=dx*cy+dz*sy;dz=-dx*sy+dz*cy;dx=xx;}
    }
    if(fighting) {
      double len=norm3(dx,dy,dz),axial=(f[0]*dx+f[1]*dy+f[2]*dz)/fmax(1e-8,len),gain=fabs(q)*shape*1.5;
      /* Factor common double expressions instead of six software divisions
       * per segment. No change to segment count, force law or integration. */
      double along=1-gain*axial,pull=len*gain;
      double nx=dx*along+pull*f[0],ny=dy*along+pull*f[1],nz=dz*along+pull*f[2];
      double factor=len/norm3(nx,ny,nz);dx=nx*factor;dy=ny*factor;dz=nz*factor;
    }
    last[0]+=dx;last[1]+=dy;last[2]+=dz;
    if(!tip_only) for(int j=0;j<3;++j) points[i][j]=last[j];
  }
  if(rail>0) {
    double ax=last[0]-points[0][0],ay=last[1]-points[0][1],az=last[2]-points[0][2];
    double span=norm3(ax,ay,az);ax/=span;ay/=span;az/=span;
    double tx=1.05-points[0][0],tz=1.85-points[0][2],ty=sqrt(fmax(.01,span*span-tx*tx-tz*tz));
    double size=norm3(tx,ty,tz),bx=ax*(1-rail)+tx/size*rail,by=ay*(1-rail)+ty/size*rail,bz=az*(1-rail)+tz/size*rail;
    size=norm3(bx,by,bz);bx/=size;by/=size;bz/=size;
    double kx=ay*bz-az*by,ky=az*bx-ax*bz,kz=ax*by-ay*bx,c=fmax(.001,1+ax*bx+ay*by+az*bz);
    last[0]=points[0][0]+bx*span;last[1]=points[0][1]+by*span;last[2]=points[0][2]+bz*span;
    if(!tip_only) for(int i=1;i<65;++i) {
      x=points[i][0]-points[0][0];y=points[i][1]-points[0][1];z=points[i][2]-points[0][2];
      double vx=ky*z-kz*y,vy=kz*x-kx*z,vz=kx*y-ky*x;
      points[i][0]=points[0][0]+x+vx+(ky*vz-kz*vy)/c;
      points[i][1]=points[0][1]+y+vy+(kz*vx-kx*vz)/c;
      points[i][2]=points[0][2]+z+vz+(kx*vy-ky*vx)/c;
    }
  }
  if(tip_only){int tip=cached_points(s,geometry,3,1,3);update_point(s,tip,1,last,3);lua_rawgeti(s,tip,1);return 1;}
  int world=cached_points(s,geometry,1,65,3),screen=cached_points(s,geometry,2,65,2);
  for(int i=0;i<65;++i) {
    update_point(s,world,i+1,points[i],3);
    z=fmax(.25,points[i][2]);double projected[]={184+260*points[i][0]/z,203+260*(1.6-points[i][1])/z};
    update_point(s,screen,i+1,projected,2);
  }
  return 2;
}

static double optional(lua_State *s,int table,const char *name,double fallback) {
  lua_getfield(s,table,name);double v=lua_isnil(s,-1)?fallback:number(s,-1);lua_pop(s,1);return v;
}
static double bounded(double x,double a,double b) {return fmax(a,fmin(b,x));}
static void set_number(lua_State *s,const char *name,double value) {lua_pushnumber(s,value);lua_setfield(s,1,name);}
static int rod_step(lua_State *s) {
  luaL_checktype(s,1,LUA_TTABLE);double dt=bounded(number(s,2),0,.25);luaL_checktype(s,3,LUA_TTABLE);
  double accumulator=field(s,"accumulator")+dt,angle=field(s,"angle"),last_speed=field(s,"last_speed");
  double length=field(s,"length_m"),ei=field(s,"EI"),omega=field(s,"omega"),damping=field(s,"damping");
  double q=field(s,"q"),v=field(s,"v");
  if(length<=0 || ei<=0 || accumulator<0 || accumulator>1) return luaL_error(s,"invalid elastic rod state");
  double target=optional(s,3,"handle_angle",-1.98),fish=fmax(0,optional(s,3,"fish_kg",0));
  double tension=fmin(fish*9.81*optional(s,3,"fish_pull",.25),optional(s,3,"drag_n",80));
  double lure=fmax(0,optional(s,3,"lure_g",0))/1000,line=fmax(0,optional(s,3,"line_n",0));
  const double h=1.0/120;
  while(accumulator>=h) {
    accumulator-=h;double delta=bounded(target-angle,-8*h,8*h);angle+=delta;
    double speed=delta/h,acceleration=bounded((speed-last_speed)/h,-120,120);last_speed=speed;
    double force=tension+line+lure*(9.81+fabs(acceleration)*length*.3);
    double raw=-force*length*length/(2*ei),equilibrium=1.5*raw/(1+fabs(raw));
    double aa=omega*omega*(equilibrium-q)-2*damping*omega*v-acceleration*.42;
    v=bounded(v+aa*h,-18,18);q=bounded(q+v*h,-1.6,1.6);
  }
  set_number(s,"accumulator",accumulator);set_number(s,"angle",angle);set_number(s,"last_speed",last_speed);
  set_number(s,"q",q);set_number(s,"v",v);lua_pushvalue(s,1);return 1;
}

static size_t read_nodes(lua_State *s,const char *name,double p[MAX_NODES][3],int *table) {
  lua_getfield(s,1,name);luaL_checktype(s,-1,LUA_TTABLE);*table=lua_gettop(s);
  size_t n=lua_rawlen(s,*table);if(n<2 || n>MAX_NODES) luaL_error(s,"invalid node count");
  for(size_t i=0;i<n;++i) {lua_rawgeti(s,*table,i+1);luaL_checktype(s,-1,LUA_TTABLE);
    for(int j=0;j<3;++j) {lua_rawgeti(s,-1,j+1);p[i][j]=number(s,-1);lua_pop(s,1);}lua_pop(s,1);}
  return n;
}
static void write_nodes(lua_State *s,int table,double p[MAX_NODES][3],size_t n) {
  for(size_t i=0;i<n;++i) {lua_rawgeti(s,table,i+1);
    for(int j=0;j<3;++j) {lua_pushnumber(s,p[i][j]);lua_rawseti(s,-2,j+1);}lua_pop(s,1);}
}
static int integrate_rope(lua_State *s) {
  luaL_checktype(s,1,LUA_TTABLE);double h=number(s,2);int released=lua_toboolean(s,3);
  double dx=number(s,4),dy=number(s,5),dz=number(s,6),distance=number(s,7),lure_drag=number(s,8);
  double mass=field(s,"mass");if(h<1e-6 || h>.1 || mass<=0) return luaL_error(s,"invalid integration");
  int landed=flag(s,"landed"),surface=flag(s,"surface"),fly=flag(s,"fly"),jump=0;
  lua_getfield(s,1,"fish");if(lua_istable(s,-1)) {lua_getfield(s,-1,"action");jump=lua_isstring(s,-1)&&strcmp(lua_tostring(s,-1),"jump")==0;lua_pop(s,1);}lua_pop(s,1);
  lua_getfield(s,1,"reel_kind");int spin=lua_isstring(s,-1)&&strcmp(lua_tostring(s,-1),"spin")==0;lua_pop(s,1);
  lua_getfield(s,1,"water");luaL_checktype(s,-1,LUA_TTABLE);int water=lua_gettop(s);
  double water_drag=optional(s,water,"drag",0),water_mass=optional(s,water,"mass",mass),acceleration=optional(s,water,"acceleration",0);
  if(water_mass<=0) return luaL_error(s,"invalid water mass");
  double p[MAX_NODES][3],prev[MAX_NODES][3];int positions,previous;
  size_t n=read_nodes(s,"p",p,&positions);if(read_nodes(s,"prev",prev,&previous)!=n) return luaL_error(s,"node mismatch");
  for(size_t i=1;i<n;++i) {
    double x=p[i][0],y=p[i][1],z=p[i][2];double ux=(x-prev[i][0])/h,uy=(y-prev[i][1])/h,uz=(z-prev[i][2])/h;
    double speed=norm3(ux,uy,uz);
    if(i+1==n && landed && y<=0) {
      double factor=1/(1+water_drag/water_mass*speed*h);ux*=factor;uy=(uy+acceleration*h)*factor;uz*=factor;
      p[i][0]=x+ux*h;p[i][1]=jump?y+uy*h:fmin(0,y+uy*h);p[i][2]=z+uz*h;if(surface)p[i][1]=0;
    } else if(i+1<n && landed && y<=0) {
      double factor=1/(1+5*h+.3*speed*h);p[i][0]=x+ux*factor*h;p[i][1]=fmin(0,y+(uy*factor+.15*h)*h);p[i][2]=z+uz*factor*h;
    } else {
      double drag=i+1==n?.000045*lure_drag/mass:.016,factor=1/(1+drag*speed*h);
      ux*=factor;uy*=factor;uz*=factor;
      if(i+1<n && y<=.005) {ux/=1+10*h;uz/=1+10*h;}
      if(released && i+1==n && !fly && distance>.1) {
        double resistance=(spin?.0015:.0028)+speed*speed*.000012,decel=fmin(speed/h,resistance/mass);
        ux-=decel*dx/distance*h;uy-=decel*dy/distance*h;uz-=decel*dz/distance*h;
      }
      p[i][0]=x+ux*h;p[i][1]=y+uy*h-9.81*h*h;p[i][2]=z+uz*h;
      if(i+1<n && p[i][1]<0 && !landed)p[i][1]=0;
    }
    prev[i][0]=x;prev[i][1]=y;prev[i][2]=z;
  }
  write_nodes(s,positions,p,n);write_nodes(s,previous,prev,n);return 0;
}
static int damp_rope(lua_State *s) {
  luaL_checktype(s,1,LUA_TTABLE);double h=number(s,2),mass=field(s,"mass"),density=field(s,"density");
  if(h<1e-6 || h>.1 || mass<=0 || density<=0) return luaL_error(s,"invalid damping");
  double blend=1-exp(-(flag(s,"fly")?14:45)*h),axial_blend=flag(s,"fish")?0:1-exp(-80*h);
  double p[MAX_NODES][3],prev[MAX_NODES][3],vel[MAX_NODES][3],rest[MAX_NODES];int positions,previous;
  size_t n=read_nodes(s,"p",p,&positions);if(read_nodes(s,"prev",prev,&previous)!=n) return luaL_error(s,"node mismatch");
  lua_getfield(s,1,"rest");luaL_checktype(s,-1,LUA_TTABLE);if(lua_rawlen(s,-1)!=n-1)return luaL_error(s,"span mismatch");
  for(size_t i=0;i+1<n;++i) {lua_rawgeti(s,-1,i+1);rest[i]=number(s,-1);lua_pop(s,1);if(rest[i]<=0)return luaL_error(s,"invalid span");}
  for(size_t i=0;i<n;++i)for(int j=0;j<3;++j)vel[i][j]=(p[i][j]-prev[i][j])/h;
  for(size_t i=1;i+1<n;++i)for(int j=0;j<3;++j) {double v=vel[i][j],mean=(vel[i-1][j]+vel[i+1][j])*.5;prev[i][j]=p[i][j]-(v+(mean-v)*blend)*h;}
  for(size_t i=0;i+1<n;++i) {
    double *a=p[i],*b=p[i+1],*ap=prev[i],*bp=prev[i+1];double x=b[0]-a[0],y=b[1]-a[1],z=b[2]-a[2],d=norm3(x,y,z);
    if(d>=rest[i]*.998 && d>1e-8) {
      double speed=((b[0]-bp[0]-a[0]+ap[0])*x+(b[1]-bp[1]-a[1]+ap[1])*y+(b[2]-bp[2]-a[2]+ap[2])*z)/(h*d);
      if(speed>0) {
        double wa=i==0?0:1/fmax(.00002,density*rest[i]),wb=i+2==n?1/mass:1/fmax(.00002,density*rest[i]);
        double impulse=speed*axial_blend/(wa+wb)*h/d,ia=wa*impulse,ib=wb*impulse;
        ap[0]-=ia*x;ap[1]-=ia*y;ap[2]-=ia*z;bp[0]+=ib*x;bp[1]+=ib*y;bp[2]+=ib*z;
      }
    }
  }
  write_nodes(s,previous,prev,n);return 0;
}

/* One substep exchanges node tables once, rather than once per kernel. */
typedef struct rope_material_cache {
  size_t n;
  double h,mass,density,axial,rest[MAX_NODES];
  float inverse[MAX_NODES],wa[MAX_NODES],wb[MAX_NODES],alpha[MAX_NODES],rest_f[MAX_NODES];
  precise_float rest_squared[MAX_NODES];
} rope_material_cache_t;
#define ROPE_MATERIAL_META "fishing.rope_material"
static uint64_t kernel_now(lua_State *s) {
  h2_runtime_t *runtime=lua_touserdata(s,lua_upvalueindex(1));uint64_t now=0;
  if(runtime)h2_pal_time_get_monotonic_us(runtime->time,&now);
  return now;
}
static int advance_rope(lua_State *s) {
  uint64_t stamp=kernel_now(s);
  luaL_checktype(s,1,LUA_TTABLE);luaL_checktype(s,2,LUA_TTABLE);
  double tip[3];for(int j=0;j<3;++j){lua_rawgeti(s,2,j+1);tip[j]=number(s,-1);lua_pop(s,1);}
  double h=number(s,3),lure_drag=number(s,5),fish_drag=number(s,7),capacity=number(s,8);
  int released=lua_toboolean(s,4),iterations=(int)luaL_checkinteger(s,6);
  double mass=field(s,"mass"),density=field(s,"density"),axial=field(s,"axial"),paid=field(s,"paid");
  if(h<1e-6 || h>.1 || iterations<1 || iterations>32 || mass<=0 || density<=0 || axial<=0 || paid<=0)
    return luaL_error(s,"invalid rope substep");
  int landed=flag(s,"landed"),surface=flag(s,"surface"),fly=flag(s,"fly"),retrieving=flag(s,"retrieving_started");
  int fish=flag(s,"fish"),jump=0,nearboat=0;
  lua_getfield(s,1,"fish");if(lua_istable(s,-1)) {
    lua_getfield(s,-1,"action");jump=lua_isstring(s,-1)&&strcmp(lua_tostring(s,-1),"jump")==0;lua_pop(s,1);
    lua_getfield(s,-1,"nearboat");nearboat=lua_toboolean(s,-1);lua_pop(s,1);
  }lua_pop(s,1);
  lua_getfield(s,1,"reel_kind");int spin=lua_isstring(s,-1)&&strcmp(lua_tostring(s,-1),"spin")==0;lua_pop(s,1);
  lua_getfield(s,1,"water");luaL_checktype(s,-1,LUA_TTABLE);int water=lua_gettop(s);
  double water_drag=optional(s,water,"drag",0),water_mass=optional(s,water,"mass",mass),acceleration=optional(s,water,"acceleration",0);
  if(water_mass<=0)return luaL_error(s,"invalid water mass");
  double p[MAX_NODES][3],prev[MAX_NODES][3],rest[MAX_NODES];
  int positions,previous;
  size_t n=read_nodes(s,"p",p,&positions);if(read_nodes(s,"prev",prev,&previous)!=n)return luaL_error(s,"node mismatch");
  lua_getfield(s,1,"rest");luaL_checktype(s,-1,LUA_TTABLE);int rests=lua_gettop(s);
  if(lua_rawlen(s,rests)!=n-1)return luaL_error(s,"span mismatch");
  for(size_t i=0;i+1<n;++i){lua_rawgeti(s,rests,i+1);rest[i]=number(s,-1);lua_pop(s,1);if(rest[i]<=0)return luaL_error(s,"invalid span");}
  lua_getfield(s,1,"lambda");luaL_checktype(s,-1,LUA_TTABLE);int lambdas=lua_gettop(s);
  uint64_t read_done=kernel_now(s);
  double dx=p[n-1][0]-tip[0],dy=p[n-1][1]-tip[1],dz=p[n-1][2]-tip[2],distance=norm3(dx,dy,dz);
  double hh=h*h;
  for(size_t i=1;i<n;++i) {
    /* Work with step displacements: velocity division and the subsequent
     * multiplication by h cancel, including quadratic drag's speed*h. */
    double x=p[i][0],y=p[i][1],z=p[i][2];
    if(i+1<n) {
      /* Small interior-node displacements use the FPU; add them to double
       * world coordinates so sub-pixel motion never rounds away at the origin. */
      float ux=(float)(x-prev[i][0]),uy=(float)(y-prev[i][1]),uz=(float)(z-prev[i][2]);
      float travel=sqrtf(ux*ux+uy*uy+uz*uz),factor;
      if(landed && y<=0) {
        factor=1.0f/(1.0f+5.0f*(float)h+.3f*travel);
        p[i][0]=x+(double)(ux*factor);p[i][1]=fmin(0,y+(double)(uy*factor)+.15*hh);p[i][2]=z+(double)(uz*factor);
      } else {
        factor=1.0f/(1.0f+.016f*travel);ux*=factor;uy*=factor;uz*=factor;
        if(y<=.005){float surface_drag=1.0f/(1.0f+10.0f*(float)h);ux*=surface_drag;uz*=surface_drag;}
        p[i][0]=x+(double)ux;p[i][1]=y+(double)uy-9.81*hh;p[i][2]=z+(double)uz;
        if(p[i][1]<0 && !landed)p[i][1]=0;
      }
      prev[i][0]=x;prev[i][1]=y;prev[i][2]=z;continue;
    }
    double ux=x-prev[i][0],uy=y-prev[i][1],uz=z-prev[i][2];
    double travel=norm3(ux,uy,uz);
    if(i+1==n && landed && y<=0) {
      double factor=1/(1+water_drag/water_mass*travel);ux*=factor;uy=(uy+acceleration*hh)*factor;uz*=factor;
      p[i][0]=x+ux;p[i][1]=jump?y+uy:fmin(0,y+uy);p[i][2]=z+uz;if(surface)p[i][1]=0;
    } else if(i+1<n && landed && y<=0) {
      double factor=1/(1+5*h+.3*travel);p[i][0]=x+ux*factor;p[i][1]=fmin(0,y+uy*factor+.15*hh);p[i][2]=z+uz*factor;
    } else {
      double drag=i+1==n?.000045*lure_drag/mass:.016,factor=1/(1+drag*travel);
      ux*=factor;uy*=factor;uz*=factor;
      if(i+1<n && y<=.005) {ux/=1+10*h;uz/=1+10*h;}
      if(released && i+1==n && !fly && distance>.1) {
        double resistance=(spin?.0015:.0028)+travel*travel/hh*.000012,decel=fmin(travel,resistance/mass*hh);
        ux-=decel*dx/distance;uy-=decel*dy/distance;uz-=decel*dz/distance;
      }
      p[i][0]=x+ux;p[i][1]=y+uy-9.81*hh;p[i][2]=z+uz;
      if(i+1<n && p[i][1]<0 && !landed)p[i][1]=0;
    }
    prev[i][0]=x;prev[i][1]=y;prev[i][2]=z;
  }

  uint64_t integrated=kernel_now(s);
  if(fish) {
    double distance=norm3(p[n-1][0]-tip[0],p[n-1][1]-tip[1],p[n-1][2]-tip[2]);
    double required=distance/(1+fish_drag/axial)-paid;
    double extra=bounded(required,0,fmin(5*h,fmax(0,capacity-paid)));
    paid+=extra;rest[0]+=extra;set_number(s,"paid",paid);set_number(s,"payout",extra/h);
    lua_pushnumber(s,rest[0]);lua_rawseti(s,rests,1);
  }
  for(int j=0;j<3;++j)p[0][j]=prev[0][j]=tip[j];
  lua_getfield(s,1,"native_material");
  rope_material_cache_t *material=luaL_testudata(s,-1,ROPE_MATERIAL_META);
  if(!material) {
    lua_pop(s,1);material=lua_newuserdatauv(s,sizeof(*material),0);memset(material,0,sizeof(*material));
    luaL_newmetatable(s,ROPE_MATERIAL_META);lua_setmetatable(s,-2);
    lua_pushvalue(s,-1);lua_setfield(s,1,"native_material");
  }
  int reset=material->n!=n || material->h!=h || material->mass!=mass || material->density!=density || material->axial!=axial;
  float *inverse_cache=material->inverse,*weight_a=material->wa,*weight_b=material->wb,*alpha_f=material->alpha,*rest_f=material->rest_f;
  precise_float *rest_squared=material->rest_squared;
  float lambda_f[MAX_NODES]={0};precise_float coordinates[MAX_NODES][3];
  for(size_t i=0;i<n;++i)for(int j=0;j<3;++j)coordinates[i][j]=pf_from(p[i][j]);
  for(size_t i=0;i+1<n;++i) if(reset || material->rest[i]!=rest[i]) {
    double node_mass=fmax(.00002,density*rest[i]);
    double wa=i==0?0:1/node_mass,wb=i+2==n?1/mass:1/node_mass,alpha=(rest[i]/axial)/(h*h);
    inverse_cache[i]=1.0f/(float)(wa+wb+alpha);
    weight_a[i]=(float)wa;weight_b[i]=(float)wb;alpha_f[i]=(float)alpha;rest_f[i]=(float)rest[i];
    rest_squared[i]=pf_from(rest[i]*rest[i]);
    material->rest[i]=rest[i];
  }
  material->n=n;material->h=h;material->mass=mass;material->density=density;material->axial=axial;
  double span_lambda=0;
  for(int iteration=0;iteration<iterations;++iteration) {
    for(size_t k=0;k+1<n;++k) {
      size_t i=iteration%2==0?k:n-2-k;
      precise_float *a=coordinates[i],*b=coordinates[i+1];
      precise_float x=pf_sub(b[0],a[0]),y=pf_sub(b[1],a[1]),z=pf_sub(b[2],a[2]);
      precise_float squared=pf_add(pf_add(pf_square(x),pf_square(y)),pf_square(z));
      precise_float difference=pf_sub(squared,rest_squared[i]);
      float gap=difference.hi+difference.lo;
      if(lambda_f[i]==0 && gap<-1e-12f*rest_squared[i].hi)continue;
      float d=sqrtf(squared.hi);if(d<1e-8f) continue;
      float wa=weight_a[i],wb=weight_b[i],alpha=alpha_f[i],old=lambda_f[i];
      /* Rationalize d-rest before converting the small strain to float. The
       * compensated numerator preserves near-taut precision without a double sqrt. */
      float strain=h2_lua_fpu_div(gap,d+rest_f[i]);
      float next=fminf(0,old+(-strain-alpha*old)*inverse_cache[i]);
      float q=h2_lua_fpu_div(next-old,d);
      lambda_f[i]=next;
      float aq=wa*q,bq=wb*q;
      if(wa>0) {a[0]=pf_add(a[0],(precise_float){-aq*x.hi,0});a[1]=pf_add(a[1],(precise_float){-aq*y.hi,0});a[2]=pf_add(a[2],(precise_float){-aq*z.hi,0});}
      if(wb>0) {b[0]=pf_add(b[0],(precise_float){bq*x.hi,0});b[1]=pf_add(b[1],(precise_float){bq*y.hi,0});b[2]=pf_add(b[2],(precise_float){bq*z.hi,0});}
    }
    if(n>2) {
      double *end=p[n-1];
      for(int j=0;j<3;++j)end[j]=(double)coordinates[n-1][j].hi+coordinates[n-1][j].lo;
      double x=end[0]-p[0][0],y=end[1]-p[0][1],z=end[2]-p[0][2];
      double distance=refined_sqrt(x*x+y*y+z*z);
      if(distance>paid) {
        double w=1/mass,alpha=paid/axial/(h*h);
        double dl=(-(distance-paid)-alpha*span_lambda)/(w+alpha);span_lambda+=dl;
        double q=w*dl/distance;end[0]+=q*x;end[1]+=q*y;end[2]+=q*z;
        for(int j=0;j<3;++j)coordinates[n-1][j]=pf_from(end[j]);
      }
    }
    if(!landed) {for(size_t i=1;i+1<n;++i) if(coordinates[i][1].hi<0)coordinates[i][1]=(precise_float){0,0};}
    else if(!retrieving && (surface || coordinates[n-1][1].hi>0))coordinates[n-1][1]=(precise_float){0,0};
  }
  for(size_t i=0;i<n;++i)for(int j=0;j<3;++j)p[i][j]=(double)coordinates[i][j].hi+coordinates[i][j].lo;
  uint64_t solved=kernel_now(s);

  if(released || landed) {
    double blend=1-exp(-(fly?14:45)*h),axial_blend=fish?0:1-exp(-80*h);
  float displacement[MAX_NODES][3],blend_f=(float)blend,axial_blend_f=(float)axial_blend;
  for(size_t i=0;i<n;++i)for(int j=0;j<3;++j)displacement[i][j]=(float)(p[i][j]-prev[i][j]);
  for(size_t i=1;i+1<n;++i)for(int j=0;j<3;++j) {float v=displacement[i][j],mean=(displacement[i-1][j]+displacement[i+1][j])*.5f;prev[i][j]-=(double)((mean-v)*blend_f);}
  for(size_t i=0;i+1<n;++i) {
    double *a=p[i],*b=p[i+1],*ap=prev[i],*bp=prev[i+1];float x=(float)(b[0]-a[0]),y=(float)(b[1]-a[1]),z=(float)(b[2]-a[2]),squared=x*x+y*y+z*z;
    if(squared>=rest_f[i]*rest_f[i]*(.998f*.998f) && squared>1e-16f) {
      float axial=(float)(b[0]-bp[0]-a[0]+ap[0])*x+(float)(b[1]-bp[1]-a[1]+ap[1])*y+(float)(b[2]-bp[2]-a[2]+ap[2])*z;
      if(axial>0) {
        float wa=weight_a[i],wb=weight_b[i];
        float impulse=axial*axial_blend_f/(wa+wb)/squared,ia=wa*impulse,ib=wb*impulse;
        ap[0]-=(double)(ia*x);ap[1]-=(double)(ia*y);ap[2]-=(double)(ia*z);bp[0]+=(double)(ib*x);bp[1]+=(double)(ib*y);bp[2]+=(double)(ib*z);
      }
    }
  }

  }
  if(fish && (nearboat || !jump) && p[n-1][1]>-.06){p[n-1][1]=-.06;prev[n-1][1]=fmin(prev[n-1][1],-.06);}
  uint64_t damped=kernel_now(s);
  write_nodes(s,positions,p,n);write_nodes(s,previous,prev,n);
  for(size_t i=0;i+1<n;++i){lua_pushnumber(s,lambda_f[i]);lua_rawseti(s,lambdas,i+1);}
  set_number(s,"tension",fmin(fish?500:8,(fabs(lambda_f[0])+fabs(span_lambda))/(h*h)));
  lua_pushinteger(s,read_done-stamp);lua_pushinteger(s,integrated-read_done);
  lua_pushinteger(s,solved-integrated);lua_pushinteger(s,damped-solved);
  lua_pushinteger(s,kernel_now(s)-damped);return 5;
}

static int verify_kernel(lua_State *s) {
  volatile double inputs[]={1.23456789,-.0123456789,13.390000001,1.00000011920928955};
  double maximum=0;
  for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
    double value=inputs[i];precise_float a=pf_from(value),square=pf_square(a);
    double error=fabs(((double)square.hi+square.lo)-value*value)/fmax(1e-8,value*value);
    if(error>maximum)maximum=error;
    precise_float near=pf_sub(pf_from(value+.00001),a);
    error=fabs(((double)near.hi+near.lo)-.00001);
    if(error>maximum)maximum=error;
  }
  double division_error=0;
  for(int i=1;i<=2048;++i) {
    float numerator=ldexpf((float)(i%127+1)/128.0f,i%60-30);
    float denominator=ldexpf((float)(i%113+1)/128.0f,(i*17)%60-30);
    double reference=(double)numerator/denominator;
    double error=fabs(h2_lua_fpu_div(numerator,denominator)-reference)/fabs(reference);
    if(error>division_error)division_error=error;
  }
  lua_pushboolean(s,maximum<1e-11 && division_error<=FLT_EPSILON);
  lua_pushnumber(s,maximum);lua_pushnumber(s,division_error);return 3;
}
int h2_lua_fishing_math_open(void *opaque, void *user) {
  lua_State *s=opaque;
  lua_newtable(s);lua_pushcfunction(s,solve_rope);lua_setfield(s,-2,"solve_rope");lua_pushcfunction(s,rod_pose);lua_setfield(s,-2,"rod_pose");lua_pushcfunction(s,rod_step);lua_setfield(s,-2,"rod_step");lua_pushcfunction(s,integrate_rope);lua_setfield(s,-2,"integrate_rope");lua_pushcfunction(s,damp_rope);lua_setfield(s,-2,"damp_rope");lua_pushlightuserdata(s,user);lua_pushcclosure(s,advance_rope,1);lua_setfield(s,-2,"advance_rope");lua_pushcfunction(s,verify_kernel);lua_setfield(s,-2,"verify_kernel");return 1;
}
