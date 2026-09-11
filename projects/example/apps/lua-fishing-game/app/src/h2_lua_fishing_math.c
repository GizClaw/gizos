#include "h2_lua_fishing_math.h"
#include "lua.h"
#include "lauxlib.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define MAX_NODES 256
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
      double d=sqrt(x*x+y*y+z*z);if(d<1e-8) continue;
      double wa=wa_cache[i],wb=wb_cache[i],alpha=alpha_cache[i],old=lambda[i];
      double next=fmin(0,old+(-(d-rest[i])-alpha*old)/(wa+wb+alpha));
      double dl=next-old;lambda[i]=next;double q=dl/d;
      if(wa>0) {a[0]-=wa*q*x;a[1]-=wa*q*y;a[2]-=wa*q*z;}
      if(wb>0) {b[0]+=wb*q*x;b[1]+=wb*q*y;b[2]+=wb*q*z;}
    }
    if(n>2) {
      double *end=p[n-1];double x=end[0]-p[0][0],y=end[1]-p[0][1],z=end[2]-p[0][2];
      double distance=sqrt(x*x+y*y+z*z);
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
} rod_geometry_t;
#define ROD_META "fishing.elastic_rod"
static double optional_field(lua_State *s,const char *name,double fallback) {
  lua_getfield(s,1,name);double v=lua_isnil(s,-1)?fallback:number(s,-1);lua_pop(s,1);return v;
}
static double norm3(double x,double y,double z) {return sqrt(x*x+y*y+z*z);}
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
    for(int i=1;i<65;++i) {
      for(int j=0;j<3;++j) g->segment[i][j]=(g->base[i][j]-g->base[i-1][j])*scale;
      g->segment[i][3]=pow(fmax(0,((double)i/64-onset)/(1-onset)),1.5);
      g->segment[i][4]=.75*g->segment[i][0]+.5*g->segment[i][1]+.4330127*g->segment[i][2];
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
  double x=g->root[0],y=g->root[1],z=g->root[2],dot=.75*x+.5*y+.4330127*z;
  points[0][0]=g->pivot[0]+x*ca+(.5*z-.4330127*y)*sa+.75*dot*(1-ca);
  points[0][1]=g->pivot[1]+y*ca+(.4330127*x-.75*z)*sa+.5*dot*(1-ca);
  points[0][2]=g->pivot[2]+z*ca+(.75*y-.5*x)*sa+.4330127*dot*(1-ca);
  double last[3]={points[0][0],points[0][1],points[0][2]};
  for(int i=1;i<65;++i) {
    double shape=g->segment[i][3],a=angle-(fighting?0:q)*shape;
    double c=(fighting || shape==0)?ca:cos(a),ss=(fighting || shape==0)?sa:sin(a);
    x=g->segment[i][0];y=g->segment[i][1];z=g->segment[i][2];dot=g->segment[i][4];
    double dx=x*c+(.5*z-.4330127*y)*ss+.75*dot*(1-c);
    double dy=y*c+(.4330127*x-.75*z)*ss+.5*dot*(1-c);
    double dz=z*c+(.75*y-.5*x)*ss+.4330127*dot*(1-c);
    if(yaw!=0){double xx=dx*cy+dz*sy;dz=-dx*sy+dz*cy;dx=xx;}
    if(fighting) {
      double len=norm3(dx,dy,dz),axial=(f[0]*dx+f[1]*dy+f[2]*dz)/fmax(1e-8,len),gain=fabs(q)*shape*1.5;
      double nx=dx+len*gain*(f[0]-axial*dx/len),ny=dy+len*gain*(f[1]-axial*dy/len),nz=dz+len*gain*(f[2]-axial*dz/len);
      double norm=norm3(nx,ny,nz);dx=nx*len/norm;dy=ny*len/norm;dz=nz*len/norm;
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
static int advance_rope(lua_State *s) {
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
  double p[MAX_NODES][3],prev[MAX_NODES][3],vel[MAX_NODES][3],rest[MAX_NODES],lambda[MAX_NODES]={0};
  int positions,previous;
  size_t n=read_nodes(s,"p",p,&positions);if(read_nodes(s,"prev",prev,&previous)!=n)return luaL_error(s,"node mismatch");
  lua_getfield(s,1,"rest");luaL_checktype(s,-1,LUA_TTABLE);int rests=lua_gettop(s);
  if(lua_rawlen(s,rests)!=n-1)return luaL_error(s,"span mismatch");
  for(size_t i=0;i+1<n;++i){lua_rawgeti(s,rests,i+1);rest[i]=number(s,-1);lua_pop(s,1);if(rest[i]<=0)return luaL_error(s,"invalid span");}
  lua_getfield(s,1,"lambda");luaL_checktype(s,-1,LUA_TTABLE);int lambdas=lua_gettop(s);
  double dx=p[n-1][0]-tip[0],dy=p[n-1][1]-tip[1],dz=p[n-1][2]-tip[2],distance=norm3(dx,dy,dz);
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

  if(fish) {
    double distance=norm3(p[n-1][0]-tip[0],p[n-1][1]-tip[1],p[n-1][2]-tip[2]);
    double required=distance/(1+fish_drag/axial)-paid;
    double extra=bounded(required,0,fmin(5*h,fmax(0,capacity-paid)));
    paid+=extra;rest[0]+=extra;set_number(s,"paid",paid);set_number(s,"payout",extra/h);
    lua_pushnumber(s,rest[0]);lua_rawseti(s,rests,1);
  }
  for(int j=0;j<3;++j)p[0][j]=prev[0][j]=tip[j];
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
      double d=sqrt(x*x+y*y+z*z);if(d<1e-8) continue;
      double wa=wa_cache[i],wb=wb_cache[i],alpha=alpha_cache[i],old=lambda[i];
      double next=fmin(0,old+(-(d-rest[i])-alpha*old)/(wa+wb+alpha));
      double dl=next-old;lambda[i]=next;double q=dl/d;
      if(wa>0) {a[0]-=wa*q*x;a[1]-=wa*q*y;a[2]-=wa*q*z;}
      if(wb>0) {b[0]+=wb*q*x;b[1]+=wb*q*y;b[2]+=wb*q*z;}
    }
    if(n>2) {
      double *end=p[n-1];double x=end[0]-p[0][0],y=end[1]-p[0][1],z=end[2]-p[0][2];
      double distance=sqrt(x*x+y*y+z*z);
      if(distance>paid) {
        double w=1/mass,alpha=paid/axial/(h*h);
        double dl=(-(distance-paid)-alpha*span_lambda)/(w+alpha);span_lambda+=dl;
        double q=w*dl/distance;end[0]+=q*x;end[1]+=q*y;end[2]+=q*z;
      }
    }
    if(!landed) {for(size_t i=1;i+1<n;++i) if(p[i][1]<0) p[i][1]=0;}
    else if(!retrieving) p[n-1][1]=surface?0:fmin(0,p[n-1][1]);
  }

  if(released || landed) {
    double blend=1-exp(-(fly?14:45)*h),axial_blend=fish?0:1-exp(-80*h);
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

  }
  if(fish && (nearboat || !jump) && p[n-1][1]>-.06){p[n-1][1]=-.06;prev[n-1][1]=fmin(prev[n-1][1],-.06);}
  write_nodes(s,positions,p,n);write_nodes(s,previous,prev,n);
  for(size_t i=0;i+1<n;++i){lua_pushnumber(s,lambda[i]);lua_rawseti(s,lambdas,i+1);}
  set_number(s,"tension",fmin(fish?500:8,(fabs(lambda[0])+fabs(span_lambda))/(h*h)));
  return 0;
}

int h2_lua_fishing_math_open(void *opaque, void *user) {
  lua_State *s=opaque; (void)user;
  lua_newtable(s);lua_pushcfunction(s,solve_rope);lua_setfield(s,-2,"solve_rope");lua_pushcfunction(s,rod_pose);lua_setfield(s,-2,"rod_pose");lua_pushcfunction(s,rod_step);lua_setfield(s,-2,"rod_step");lua_pushcfunction(s,integrate_rope);lua_setfield(s,-2,"integrate_rope");lua_pushcfunction(s,damp_rope);lua_setfield(s,-2,"damp_rope");lua_pushcfunction(s,advance_rope);lua_setfield(s,-2,"advance_rope");return 1;
}
