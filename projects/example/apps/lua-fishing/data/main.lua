-- AMOLED 368x448 visual prototype. All art is authored geometry drawn in Lua.
-- No image loader, sprite atlas, framebuffer asset, or external font is used.
local D = require('display')
local delay = require('delay')
local system = require('system')
local touch = require('lcd_touch')
local A = args or {}
local floor, sin, cos, pi = math.floor, math.sin, math.cos, math.pi
local function clamp(x,a,b) return math.max(a,math.min(b,x)) end
local function rgb(hex) return {r=floor(hex/65536)%256,g=floor(hex/256)%256,b=hex%256} end
local C={sky=rgb(0x29b6fd),sea1=rgb(0x0072d3),sea2=rgb(0x005ebc),sea3=rgb(0x005cba),
 white=rgb(0xf7ffff),foam=rgb(0xbceef5),bluefoam=rgb(0x32b8ed),ink=rgb(0x102532),
 gold=rgb(0xf9b83c),shine=rgb(0xffd976),grip=rgb(0x343c42),gray=rgb(0x809096),
 silver=rgb(0xb8c7c8),red=rgb(0xe85137),cork=rgb(0xd8ac70),green=rgb(0x88dc2d),
 wood=rgb(0x78512e),darkwood=rgb(0x53391f),cream=rgb(0xf3dfad),cyan=rgb(0x91dfec)}
local function rect(x,y,w,h,c)
 x,y,w,h=floor(x+.5),floor(y+.5),floor(w+.5),floor(h+.5)
 local right,bottom=math.min(368,x+w),math.min(448,y+h)
 x,y=math.max(0,x),math.max(0,y)
 if right>x and bottom>y then D.fill_rect(x,y,right-x,bottom-y,c) end
end
local function line(x,y,xx,yy,c)
 D.draw_line(floor(x+.5),floor(y+.5),floor(xx+.5),floor(yy+.5),c)
end
-- Pixel scanline polygon; flat color, no antialiasing, no texture.
local function poly(p,c)
 local lo,hi=448,0
 for _,v in ipairs(p) do lo=math.min(lo,v[2]);hi=math.max(hi,v[2]) end
 for y=math.max(0,math.ceil(lo)),math.min(447,floor(hi)) do
  local xs={}
  for i,a in ipairs(p) do local b=p[i%#p+1]
   if (a[2]<=y and b[2]>y) or (b[2]<=y and a[2]>y) then xs[#xs+1]=a[1]+(y-a[2])*(b[1]-a[1])/(b[2]-a[2]) end
  end
  table.sort(xs)
  for i=1,#xs-1,2 do rect(math.ceil(xs[i]),y,floor(xs[i+1])-math.ceil(xs[i])+1,1,c) end
 end
end
local function stroke(x,y,xx,yy,w,c)
 local dx,dy=xx-x,yy-y;local len=math.sqrt(dx*dx+dy*dy)
 if len<.01 then rect(x-w/2,y-w/2,w,w,c);return end
 local nx,ny=-dy/len*w/2,dx/len*w/2
 poly({{x+nx,y+ny},{xx+nx,yy+ny},{xx-nx,yy-ny},{x-nx,y-ny}},c)
 -- Center line connects very thin, steep pixel steps.
 line(x,y,xx,yy,c)
end
local function border(x,y,w,h,c,n)
 n=n or 1;rect(x,y,w,n,c);rect(x,y+h-n,w,n,c);rect(x,y,n,h,c);rect(x+w-n,y,n,h,c)
end
local function ellipse(x,y,rx,ry,c)
 for yy=-ry,ry do local ex=rx*math.sqrt(math.max(0,1-yy*yy/(ry*ry)));rect(x-ex,y+yy,2*ex+1,1,c) end
end
-- 5x7 bitmap letter definitions are font glyphs, never image assets.
local glyphs={
 A='0e11111f111111',B='1e11111e11111e',C='0e11101010110e',D='1e11111111111e',E='1f10101e10101f',F='1f10101e101010',
 G='0e11101711110f',H='1111111f111111',I='0e04040404040e',J='0702020212120c',K='11121418141211',L='1010101010101f',
 M='111b1515111111',N='11191513111111',O='0e11111111110e',P='1e11111e101010',Q='0e11111115120d',R='1e11111e141211',
 S='0f10100e01011e',T='1f040404040404',U='1111111111110e',V='11111111110a04',W='11111115151b11',X='11110a040a1111',
 Y='11110a04040404',Z='1f01020408101f',
 ['0']='0e11131519110e',['1']='040c040404040e',['2']='0e11010204081f',['3']='1e01010601011e',['4']='02060a121f0202',
 ['5']='1f10101e01011e',['6']='0e10101e11110e',['7']='1f010204080808',['8']='0e11110e11110e',['9']='0e11110f01010e',
 ["'"]='04040800000000',['"']='0a0a0000000000',['.']='00000000000c0c',['/']='01020204080810',['-']='0000001f000000',
 [':']='000c0c000c0c00',['+']='0004041f040400',['<']='02040810080402',['>']='08040201020408',
}
local lowercase={
 a='00000e010f110f',b='1010161911111e',c='00000e1110110e',d='01010d1311110f',e='00000e111f100e',
 f='0609091c080808',g='00000f11110f01',h='10101619111111',i='04000c0404040e',j='0200060202120c',
 k='101011121c1211',l='0c04040404040e',m='00001a15151515',n='00001619111111',o='00000e1111110e',
 p='00001e11111e10',q='00000f11110f01',r='00001619101010',s='00000f100e011e',t='08081c08080906',
 u='0000111111130d',v='00001111110a04',w='0000111115150a',x='0000110a040a11',y='00001111110f01',z='00001f0204081f',
}
for k,v in pairs(lowercase) do glyphs[k]=v end
local function text(x,y,s,c,scale,bold)
 scale=scale or 1
 for ch in s:gmatch('.') do
  local rows=glyphs[ch]
  if rows then
   for row=0,6 do local bits=tonumber(rows:sub(row*2+1,row*2+2),16)
    for col=0,4 do if floor(bits/2^(4-col))%2==1 then rect(x+col*scale,y+row*scale,scale+(bold and .8 or 0),scale,c) end end
   end
  end
  x=x+6*scale
 end
end
local function centered(x,y,w,s,c,scale) text(x+(w-(#s*6-1)*scale)/2,y,s,c,scale,true) end
local brands={'SHIMANO','DAIWA','ABU GARCIA','MEGABASS','GAMAKATSU','SAGE','ORVIS','RAPALA','KEITECH','JACKALL','HEDDON','CUSTOM'}
-- Manufacturer labels are immutable. sim_power/action and rig weights are game tuning.
local rods={
 {kind='lure',mount='bait',feet=6+8/12,label="6'8",power='M',action='RF',brand=1,name='BANTAM',model='168M',mass='117G',rating='7-21G / 8-16LB'},
 {kind='lure',mount='spin',feet=8,label="8'0",power='ML',action='N/A',sim_action='RF',brand=1,name='LUNAMIS',model='S80ML',mass='111G',rating='6-25G / PE 0.5-1.5'},
 {kind='lure',mount='bait',feet=6+8/12,label="6'8",power='M',action='R',brand=1,name='EXPRIDE',model='168M-LM/2',mass='130G',rating='7-30G / 8-16LB'},
 {kind='lure',mount='spin',feet=2.62/.3048,label="8'7",power='LML',sim_power='ML',action='N/A',sim_action='RF',brand=2,name='MORETHAN BRANZINO EX AGS',model='87LML',mass='97G',rating='5-30G / PE 0.6-1.5'},
 {kind='iso',mount='spin',feet=5.3/.3048,label='5.3M',power='M',action='DEEP',sim_power='ML',sim_action='R',brand=5,name='MASTER MODEL II KUCHIBUTO',model='M-53',mass='235G',rating='SINKER 1-3 / LEADER 1-2.75'},
 {kind='lure',mount='bait',feet=6.5,label="6'6",power='M',action='RF',brand=2,name='STEEZ',model='C66M TYPE-1.5',mass='93G',rating='5-21G / 8-16LB'},
 {kind='fly',mount='fly',feet=9,label="9'0",power='8WT',wt=8,sim_power='M',action='F',brand=6,name='SALT R8',model='890-4',mass='N/A',rating='8WT / 4 PIECES'},
 {kind='lure',mount='bait',feet=7,label="7'0",power='MH',action='XF',brand=3,name='VERITAS',model='VRPC70-6',mass='N/A',rating='1/4-1OZ / 12-20LB'},
 {kind='lure',mount='bait',feet=7+2/12,label="7'2",power='H',action='XF',brand=4,name='DESTROYER P5 THE X-BITES',model='F5.5-72X',mass='108G',rating='1/4-1OZ / 10-25LB'},
}
local reels={
 {kind='bait',brand=1,name='ANTARES DC MD',model='HG RIGHT',capacity='NYLON 16LB / 120M',spec='7.4:1  235G  DRAG 6KG'},
 {kind='bait',brand=3,name='REVO SX GEN 5',model='REVO5 SX LP-L',capacity='MONO 12LB / 130YD',spec='6.7:1  DRAG 25LB  LEFT'},
 {kind='spin',brand=1,name='STELLA FK',model='C3000 / NZ',capacity='PE 1.5 / 270M',spec='5.1:1  210G  DRAG 9KG'},
 {kind='spin',brand=1,name='TWINPOWER',model='C3000 / 046802',capacity='PE 1.5 / 270M',spec='5.1:1  215G  DRAG 9KG'},
 {kind='fly',brand=7,name='HYDROS 2026',model='III',wtmin=5,wtmax=7,capacity='WF6 + BACKING 125YD',spec='5-7WT  6.1OZ  DIA 3.88IN'},
 {kind='fly',brand=7,name='HYDROS 2026',model='IV',wtmin=7,wtmax=9,capacity='WF8 + BACKING 200YD',spec='7-9WT  6.8OZ  DIA 4.25IN'},
 {kind='spin',brand=2,name='22 EXIST',model='LT2500S',capacity='PE 0.6 / 200M',spec='5.1:1  160G  DRAG 5KG'},
 {kind='spin',brand=2,name='24 LUVIAS',model='LT2500S',capacity='PE 0.6 / 200M',spec='5.1:1  150G  DRAG 5KG'},
 {kind='bait',brand=2,name='24 STEEZ SV TW',model='100H',capacity='PE 1.5 / 120M',spec='7.8:1  160G  DRAG 5KG'},
 {kind='round',brand=1,name='CALCUTTA CONQUEST',model='100 RIGHT / 042323',capacity='NYLON 12LB / 100M',spec='5.6:1  220G  DRAG 4.5KG'},
 {kind='round',brand=1,name='CALCUTTA CONQUEST',model='200HG RIGHT / 042385',capacity='NYLON 16LB / 120M',spec='6.5:1  240G  DRAG 6KG'},
}
local lures={
 {kind='lure',brand=1,name='SILENT ASSASSIN FB',model='99F / XM-199V',icon=1,weight=14,length='99MM',spec='14G / FLOAT / HOOK 5 X2',drag=.9},
 {kind='lure',brand=2,name='VERTICE R',model='125F-SSR',icon=1,weight=20.3,length='125MM',spec='20.3G / SLOW FLOAT',drag=.9},
 {kind='lure',brand=9,name='FAT SWING IMPACT',model='3.8 IN',icon=3,weight=5,weight_estimated=true,length='96.5MM',spec='WEIGHT N/A / HOOK 2/0',drag=.75},
 {kind='lure',brand=10,name='CHUBBY',model='38F',icon=4,weight=4,length='38MM',spec='4G / FLOAT / HOOK 10',drag=1.45},
 {kind='lure',brand=4,name='POP-X',model='POP-X',icon=5,weight=7.087,length='2.5IN',spec='1/4OZ / TOPWATER',surface=true,drag=1.1},
 {kind='lure',brand=11,name='SUPER SPOOK JR.',model='X9236',icon=6,weight=14.175,length='3.5IN',spec='1/2OZ / TOPWATER',surface=true,drag=.65},
 {kind='iso',brand=12,name='A-WA FLOAT RIG',model='PROTOTYPE',icon=7,weight=5,length='N/A',spec='SIMULATION RIG',drag=.5},
 {kind='fly',brand=7,name='CLOUSER MINNOW',model='02JX / HOOK 2',icon=8,weight=1,weight_estimated=true,length='N/A',spec='WEIGHT N/A / STREAMER',drag=.4},
 {kind='lure',brand=4,name='VISION ONETEN SW',model='SW',icon=1,weight=14.175,length='4.33IN',spec='1/2OZ / SUSPEND / 4FT',drag=.95},
 {kind='lure',brand=8,name='ORIGINAL FLOATING',model='F07 / US',icon=2,weight=3.544,length='2.75IN',spec='1/8OZ / FLOAT / 3-5FT',drag=.7},
}
local powers={'UL','L','ML','M','MH','H','XH','XXH','XXXH'}
local actions={'R','RF','F','XF'}
local stiffness={UL=.55,L=.75,ML=1,M=1.25,MH=1.6,H=2,XH=2.5,XXH=3.1,XXXH=3.8}
local action_start={R=.12,RF=.28,F=.48,XF=.64}
local function compatible(rod,item,is_reel)
 if not item then return false end
 if not is_reel then return rod.kind==item.kind end
 return (rod.kind=='lure' and ((rod.mount=='bait' and (item.kind=='bait' or item.kind=='round')) or (rod.mount=='spin' and item.kind=='spin'))) or
        (rod.kind=='iso' and item.kind=='spin') or (rod.kind=='fly' and item.kind=='fly' and rod.wt>=item.wtmin and rod.wt<=item.wtmax)
end
local rod_index=tonumber(A.rod) or 1
local reel_index=tonumber(A.reel) or 1
local lure_index=1
local rod=rods[rod_index]
-- Real catalog specs are immutable; CLI visual overrides are intentionally unused.
local function reconcile()
 if not compatible(rod,reels[reel_index],true) then reel_index=nil end
 if not compatible(rod,lures[lure_index],false) then lure_index=nil end
end
reconcile()
local scene=A.scene or 'idle'
if scene=='iso' then rod_index=5;rod=rods[5];reel_index=4;lure_index=7
elseif scene=='fly-back' or scene=='fly-send' then rod_index=7;rod=rods[7];reel_index=6;lure_index=8 end
local menu=scene=='rods' or scene=='reels' or scene=='lures'
local tab=scene=='reels' and 2 or (scene=='lures' and 3 or 1)
local focus={rod_index,reel_index or 1,lure_index or 1}
local pages={0,floor(((reel_index or 1)-1)/9),0}
local detail_page=0
local fixed=tonumber(A.time_ms)
local start=system.millis()
local cast_start=nil
local cycle={'overhead','pendulum','iso','fly-back','fly-send','fight'}
local function logo(brand,x,y)
 if brand==1 then text(x,y,'SHIMANO',C.cyan,2.5,true)
 elseif brand==2 then
  poly({{x,y},{x+19,y},{x+27,y+9},{x+16,y+19},{x+1,y+19},{x+14,y+8},{x,y+8}},C.white)
  poly({{x+24,y},{x+31,y},{x+41,y+9},{x+29,y+19},{x+20,y+19},{x+32,y+9}},C.white)
  text(x+48,y,'DAIWA',C.white,2.5,true)
 elseif brand==3 then
  rect(x,y,23,6,C.red);rect(x-2,y+8,25,3,C.red)
  text(x+29,y,'Abu',C.white,2);text(x+29,y+17,'Garcia',C.white,1)
 else text(x,y,brands[brand] or 'CUSTOM',C.cream,2,true) end
end
-- Tiny reel silhouettes, mounted in rod-local coordinates (u towards the tip,
-- v to its upper side). This keeps positions correct as the rod rotates.
local function reel_icon(kind,x,y,scale,angle,disabled)
 local u,v=math.cos(angle or 0),math.sin(angle or 0)
 local function p(a,b) return {x+(a*u-b*v)*scale,y+(a*v+b*u)*scale} end
 local dark=disabled and rgb(0x605545) or C.ink
 local mid=disabled and rgb(0x75654f) or C.gray
 local light=disabled and rgb(0x887456) or C.silver
 local function box(points,c) local pp={};for _,q in ipairs(points) do pp[#pp+1]=p(q[1],q[2]) end;poly(pp,c) end
 local function l(a,b,c,d,color) local s,e=p(a,b),p(c,d);stroke(s[1],s[2],e[1],e[2],math.max(1,floor(scale)),color) end
 if kind=='round' then
  l(-2,0,2,0,dark);l(0,0,0,-2,mid)
  local function disc(cx,cy,r,color)
   local q={};for i=0,15 do local a=i*pi/8;q[#q+1]=p(cx+cos(a)*r,cy+sin(a)*r) end;poly(q,color)
  end
  disc(-1,-6,5,dark);disc(1,-6,4.5,C.gold);disc(1,-6,3,rgb(0xcfaa55))
  l(-1,-9,1,-10,C.cream)
  l(4,-4,7,-3,C.gold);l(7,-3,8,-7,light);l(7,-3,6,1,light)
  l(8,-7,10,-7,dark);l(6,1,8,1,dark)
 elseif kind=='bait' then
  box({{-4,-1},{-5,-5},{-2,-8},{4,-7},{6,-4},{4,0}},dark)
  box({{-3,-3},{-3,-5},{2,-6},{4,-4},{2,-2}},mid)
  l(-1,-5,2,-5,light);l(4,-3,8,-2,dark);l(8,-5,8,1,dark)
  box({{7,-6},{10,-6},{10,-4},{7,-4}},dark);box({{7,0},{10,0},{10,2},{7,2}},dark)
  l(-2,1,-3,3,dark)
 elseif kind=='spin' then
  l(0,0,0,5,dark);l(0,5,3,8,dark)
  box({{-3,6},{4,6},{6,9},{3,12},{-3,11}},dark)
  box({{-2,7},{2,7},{2,10},{-2,10}},mid);l(3,5,6,6,light);l(6,6,6,11,light)
  l(-2,10,-6,13,dark);l(-6,13,-8,12,dark)
 else
  box({{-3,0},{-3,3},{-4,4},{-4,9},{-1,11},{2,9},{2,4},{0,2},{0,0}},dark)
  box({{-2,4},{0,3},{1,5},{1,8},{-1,9},{-2,7}},mid)
  l(-2,4,-2,7,light)
 end
end
-- Authored pixel geometry: layered bodies, hardware and hanging treble hooks.
local function lure_icon(index,x,y,scale,disabled,angle)
 local ca,sa=cos(angle or 0),sin(angle or 0)
 local function col(c)
  if not disabled then return c end
  local v=c.r*.3+c.g*.5+c.b*.2
  return {r=floor(67+v*.24),g=floor(59+v*.20),b=floor(46+v*.17)}
 end
 local function p(q,c)
  local v={};for _,z in ipairs(q) do v[#v+1]={x+(z[1]*ca-z[2]*sa)*scale,y+(z[1]*sa+z[2]*ca)*scale} end;poly(v,col(rgb(c)))
 end
 local function l(a,b,c,d,k) local ax,ay=x+(a*ca-b*sa)*scale,y+(a*sa+b*ca)*scale;local bx,by=x+(c*ca-d*sa)*scale,y+(c*sa+d*ca)*scale;line(ax,ay,bx,by,col(rgb(k))) end
 local function dot(a,b,r,k) ellipse(x+(a*ca-b*sa)*scale,y+(a*sa+b*ca)*scale,r*scale,r*scale,col(rgb(k))) end
 local function hook(a,b)
  dot(a,b,.65,0x8f9996);l(a,b+1,a,b+9,0x707c7c);l(a+.5,b+2,a+.5,b+8,0xe0e4d4)
  l(a,b+9,a-3,b+9,0xc2cac0);l(a-3,b+9,a-3.5,b+7,0xc2cac0);l(a-3.5,b+7,a-3,b+6,0xe6e7d7)
  l(a,b+9,a+2.5,b+8.5,0xc2cac0);l(a+2.5,b+8.5,a+3,b+6,0xe6e7d7);l(a,b+9,a+1,b+7,0x9da69f)
 end
 if index==7 then
  p({{-4,0},{-3,-5},{0,-8},{3,-5},{4,0},{2,5},{-2,5}},0xb12b16)
  p({{-3,-3},{0,-6},{2,-4},{2,0},{-3,0}},0xff7533);l(-2,-3,-2,0,0xffe7a2)
  l(0,-11,0,-8,0x292c28);l(0,5,0,11,0xb5bbaa);l(-3,1,3,1,0xe6dfb9)
 elseif index==8 then
  for i=-4,4 do
   l(-9,0,13, i*1.5,0x9aab9b);l(-5,0,10,i,0xd3cdb4)
   l(1,i*.45,7,i*1.3,0x7e9485)
  end
  p({{-11,0},{-7,-2},{-3,-1},{-3,2},{-8,2}},0xb16d36)
  dot(-9,0,1,0xe8ddba);dot(-9,0,.45,0x242f2b)
  l(-8,1,5,2,0xb7bdb4);l(5,2,6,6,0xb7bdb4);l(6,6,3,7,0xb7bdb4);l(3,7,2,5,0xb7bdb4)
 else
  if index==1 then
   p({{-16,0},{-13,-3},{-6,-4},{3,-4},{10,-2},{16,-1},{17,1},{10,2},{-8,3},{-14,2}},0x536a75)
   p({{-13,-2},{-6,-4},{3,-4},{10,-2}},0x267da4)
   p({{-14,0},{-8,-2},{9,-1},{15,0},{10,2},{-9,2}},0xa8c3cc)
   l(-12,1,11,1,0xedf4ed);l(-8,-2,7,-2,0x75bad3);l(-7,0,9,0,0xd3e2e1)
   p({{-14,1},{-18,5},{-16,5},{-12,2}},0xc0d2cd)
   l(-11,-1,-11,1,0xbf5647);dot(-13,-.6,1,0xd9d7c6);dot(-13,-.6,.5,0x263a40)
   hook(-6,3);hook(17,1)
  elseif index==2 then
   p({{-17,0},{-15,-2},{-9,-3},{7,-2},{14,-1},{17,0},{12,2},{-10,3},{-16,2}},0x9e9b80)
   p({{-16,-1},{-9,-2},{7,-1},{14,0},{9,1},{-15,1}},0xe7e2c7)
   l(-12,2,10,1,0xc8c7af);l(-10,-1,5,-1,0xf7f4de)
   dot(-14,0,1.3,0xbc3e29);dot(-14,0,.65,0x292c23);l(-12,-1,-12,1,0xfcf2cf)
   hook(-9,3);hook(17,0)
  elseif index==3 then
   p({{-17,0},{-13,-3},{-4,-3},{4,-2},{11,-1},{14,-2},{15,-5},{18,-6},{18,5},{15,4},{14,1},{6,1},{1,3},{-9,4},{-15,2}},0x6f9d12)
   p({{-16,0},{-11,-2},{-4,-2},{4,-1},{12,0},{3,1},{-3,3},{-11,2}},0xb5e617)
   l(-10,0,2,0,0xd7f62e);l(-9,2,-1,2,0x91c112)
   p({{15,-3},{17,-4},{17,3},{15,2}},0xade51d)
   dot(-13,0,1.7,0xdcefa5);dot(-13,0,.85,0x253e19);dot(-13.4,-.5,.3,0xffffff)
  elseif index==4 then
   p({{-12,0},{-10,-3},{-6,-6},{-1,-6},{6,-3},{14,1},{7,3},{-7,4}},0x8b642d)
   p({{-11,-1},{-6,-5},{-1,-5},{6,-2},{12,1},{-8,1}},0xd78b25)
   p({{-10,1},{11,1},{5,3},{-7,3}},0xe1d8a6)
   l(-5,-4,2,-3,0xf1ad3d);l(-4,-2,7,0,0xeaa832);l(-7,0,-5,0,0xc46821)
   p({{-11,1},{-20,8},{-17,9},{-9,5},{-7,2}},0xb7c9c7);l(-18,7,-11,2,0xf2f3df)
   dot(-9,-1,1.8,0xf5e0a3);dot(-9,-1,1,0x434238);dot(-9.4,-1.4,.3,0xffffff)
   hook(-4,4);hook(16,1)
  elseif index==5 then
   p({{-14,-5},{-10,-5},{-3,-3},{6,1},{15,6},{7,5},{-4,1},{-13,-2}},0x8f9995)
   p({{-12,-4},{-9,-4},{-2,-2},{7,2},{13,5},{2,2},{-11,-1}},0xe1e5df)
   l(-9,-3,6,2,0xffffff);l(-8,0,7,4,0xb4c0c2)
   p({{-14,-5},{-11,-4},{-11,-1},{-14,-2}},0xc63828)
   dot(-12.5,-3, .6,0x242d2a);l(-22,-5,-15,-4,0xc2ccc3);l(-22,-6,-16,-6,0x8b9690)
   hook(-6,1);hook(17,6)
  else
   p({{-17,1},{-13,-2},{-6,-4},{1,-4},{8,-1},{15,4},{8,3},{-7,4},{-15,3}},0x748f16)
   p({{-16,1},{-11,-1},{-4,-3},{1,-3},{8,0},{12,2},{-8,3}},0xc5df24)
   p({{-15,2},{-7,2},{1,1},{9,2},{6,3},{-8,4}},0xebdc6c)
   l(-8,-2,1,-2,0xe5ed54);l(-8,-1,-8,1,0xeb8042)
   dot(-13,1,1.6,0xeaf1a6);dot(-13,1,.85,0x27371b)
   hook(-5,4);hook(17,4)
  end
 end
end
-- Larger inventory hardware uses independent pixel geometry, not a scaled icon.
local function inventory_reel(kind,x,y,disabled,accent,scale)
 scale=scale or 1
 local function col(c)
  local k=rgb(c);if not disabled then return k end
  local v=k.r*.3+k.g*.5+k.b*.2
  return {r=floor(67+v*.24),g=floor(59+v*.20),b=floor(46+v*.17)}
 end
 local function P(q,c) local v={};for _,z in ipairs(q) do v[#v+1]={x+z[1]*scale,y+z[2]*scale} end;poly(v,col(c)) end
 local function L(a,b,c,d,w,k) stroke(x+a*scale,y+b*scale,x+c*scale,y+d*scale,w*scale,col(k)) end
 local function E(a,b,rx,ry,c) ellipse(x+a*scale,y+b*scale,rx*scale,ry*scale,col(c)) end
 local ac=accent==C.red and 0xd53535 or (accent==C.gold and 0xd5b64b or (accent==C.cyan and 0x2b96d1 or 0x768185))
 if kind=='round' then
  -- Circular side elevation. The offset back rim conveys housing thickness;
  -- the near circular cover hides the spool instead of exposing a second face.
  E(-10,-4,22,22,0x735826);E(-10,-5,20,20,0xb68d3b)
  L(-23,-18,-13,-24,2,0xf0d18a)
  P({{-10,-26},{1,-23},{19,-11},{21,7},{4,23},{-10,18}},0xab8132)
  -- Reel foot is fixed to the frame below the drum.
  P({{-10,18},{1,18},{3,24},{-1,28},{-16,28},{-15,25}},0xa17b32)
  L(-13,25,-2,25,2,0xe4c47b)
  E(-2,0,24,24,0x73531f);E(-3,-1,22,22,0xd8b35f)
  E(-3,-1,19,19,0xbd9340);E(-4,-2,18,18,0xd2ac56)
  -- Upper highlight and lower rim share one circular centre.
  for i=0,20 do local a=pi+i*pi/40;local b=pi+(i+1)*pi/40
   L(-3+cos(a)*21,-1+sin(a)*21,-3+cos(b)*21,-1+sin(b)*21,1,0xf4d896)
  end
  E(-10,-13,1,1,0x7b622c);E(7,12,1,1,0x7b622c)
  L(-13,-3,1,-3,1,0xa07a31);L(-10,0,-1,0,1,0xa07a31)
  -- Offset drive gear: shaft, star drag and crank are connected.
  E(12,5,8,10,0xa47b2d);E(12,4,6,8,0xe1bd69)
  E(8,-10,4,4,0x806126);E(8,-11,3,3,0xe1c580)
  L(14,5,27,7,5,0x9d7a31)
  for i=0,4 do local a=i*2*pi/5
   L(23,6,23+cos(a)*5,6+sin(a)*7,2,0xd6b362)
  end
  E(24,6,3,3,0x6c562b)
  L(28,6,31,-19,3,0xc4b683);L(28,6,25,31,3,0xc4b683)
  L(29,3,32,-18,1,0xf5e7b2);L(27,10,26,29,1,0xf5e7b2)
  E(29,6,2,3,0x413f2d)
  L(31,-19,35,-18,3,0x897d55);L(25,31,29,32,3,0x897d55)
  E(36,-18,6,4,0x242e2a);E(32,32,7,4,0x242e2a)
  L(34,-20,38,-20,1,0x5a6559);L(30,30,34,30,1,0x5a6559)
 elseif kind=='bait' then
  P({{-24,3},{-22,-11},{-12,-23},{0,-25},{11,-20},{18,-9},{16,9},{8,18},{-8,20},{-21,14}},0x232829)
  P({{-22,1},{-19,-11},{-10,-21},{0,-23},{9,-18},{11,-8},{6,9},{-5,16},{-16,14}},0x515b5d)
  P({{-18,-2},{-13,-13},{-5,-18},{4,-16},{7,-10},{2,9},{-9,12},{-17,8}},0x20272a)
  -- Open frame and cylindrical spool, seen obliquely.
  P({{-14,-9},{-3,-13},{5,-10},{4,-3},{-7,0},{-16,-3}},0x8b9290)
  L(-14,-7,3,-3,3,0xc0c2b5);L(-14,-5,2,-1,2,0x656b67)
  L(-13,-3,1,0,1,0xe2dbc3);L(-5,-11,-7,-1,1,0x969c94)
  P({{-19,0},{-13,3},{-12,13},{-17,11}},0x7e8888)
  P({{6,-17},{13,-13},{16,-3},{12,13},{5,17},{2,10}},0x383e41)
  P({{9,-9},{14,-6},{13,4},{8,10},{5,8},{6,-2}},0x535c5c)
  E(10,2,3,5,0x282f32);L(8,-7,11,-9,1,0x949b94)
  L(-21,-4,-18,-12,3,ac);L(-18,-12,-11,-20,3,ac)
  L(1,-21,7,-17,3,ac);L(7,-17,4,-7,3,ac);L(4,-7,1,9,3,ac);L(1,9,3,16,3,ac)
  L(-17,-10,-12,-16,1,0xb1b6ae);L(-20,2,-18,9,1,0x979e98)
  L(-7,17,-8,23,3,0x303538)
  -- Star drag, offset crank and two distinct paddle grips.
  L(14,1,21,-2,3,0x737b76);E(17,0,3,3,0x252e30)
  L(18,-4,18,4,1,0x92958b);L(14,-1,22,1,1,0x92958b)
  L(20,-1,24,-16,3,0x242b2e);L(20,-1,17,23,3,0x242b2e)
  L(21,-3,24,-13,1,0x737b79);L(19,6,17,20,1,0x737b79)
  P({{23,-20},{32,-20},{36,-17},{35,-12},{31,-10},{23,-11},{21,-14}},0x252b2d)
  L(26,-18,32,-18,2,0x41494b)
  P({{17,19},{27,20},{30,23},{28,28},{21,29},{16,26},{14,23}},0x252b2d)
  L(19,21,25,22,2,0x41494b)
 elseif kind=='spin' then
  L(-6,-27,16,-27,4,0x333b3d);L(-6,-28,15,-28,2,0xa6aeaa)
  P({{2,-25},{7,-25},{7,-16},{13,-7},{10,-2},{3,-10}},0x8d9693)
  L(3,-24,3,-16,1,0xd2d4c7)
  P({{-4,-7},{10,-6},{17,2},{17,11},{8,17},{-4,14},{-10,6}},0x737d7b)
  P({{-3,-6},{8,-5},{14,1},{12,8},{3,10},{-5,5}},0xb4bbb4)
  E(9,5,4,4,0x454e50);L(7,3,10,3,1,0x222a2d)
  -- Spool lip, wound line, band and rotor.
  P({{-24,-6},{-18,-10},{-7,-9},{-4,-5},{-5,10},{-12,14},{-22,12},{-25,8}},0x313b3f)
  P({{-21,-5},{-17,-8},{-7,-7},{-7,10},{-16,12},{-21,9}},0x9da9a8)
  L(-18,-5,-18,10,4,ac);L(-17,-5,-17,9,1,0xefdf91)
  L(-12,-6,-12,9,3,0xc7d0c8);L(-10,-5,-10,8,1,0x939f9e)
  L(-23,-5,-23,9,2,0x737e7f);L(-25,-3,-25,7,1,0xc5ccc3)
  L(-6,-3,-6,8,2,0xe0e0d1)
  -- Thin bail wire wraps around the spool; rotor runs below it.
  L(-20,-15,-12,-12,2,0xb8c5c1);L(-12,-12,5,-12,2,0xb8c5c1);L(5,-12,17,-3,2,0xb8c5c1)
  P({{-1,12},{5,13},{-5,24},{-19,24},{-19,21},{-7,20}},0x9aa9a5)
  L(-17,22,-7,22,1,0xd3dbcc)
  L(14,10,22,18,3,0x343f42);L(22,18,29,9,3,0x929e99)
  P({{28,3},{32,2},{36,6},{34,10},{28,16},{24,12},{24,9}},0x283337)
  L(29,5,32,5,2,0x485558)
 else
  L(-11,-26,13,-26,3,0x9da9a5);L(1,-25,1,-17,4,0x727d79)
  E(1,3,23,24,0x343e3d);E(-1,2,21,23,0x9ba49a);E(-2,2,18,20,0x7c8980)
  for i=0,7 do local a=i*pi/4;E(-2+cos(a)*13,2+sin(a)*14,3.6,4,0x303a36) end
  E(-2,2,6,6,0x3b4741);E(-2,2,2,2,0xb0b8a7);E(12,13,2,3,0xc0c5af)
 end
end
local function rod_thumbnail(item,x,y)
 local sx,sy,tx,ty=x+13,y+73,x+65,y+22
 local color=({rgb(0x667d83),rgb(0x447b50),rgb(0x608aa0)})[(item.brand-1)%3+1]
 stroke(sx,sy,tx,ty,3,C.ink);stroke(sx+5,sy-5,tx,ty,1.5,color)
 stroke(sx,sy,sx+11,sy-11,7,C.ink)
 stroke(sx+1,sy-1,sx+10,sy-10,5,item.kind=='iso' and C.silver or C.cork)
 for n=1,6 do local u=n/7;local xx,yy=sx+(tx-sx)*u,sy+(ty-sy)*u
  line(xx-1,yy-1,xx+1,yy+1,n%2==0 and C.silver or C.ink)
 end
end
local function wood(y)
 local colors={0x79512e,0x734b2b,0x7b512e,0x754c2b,0x7e542f,0x79512e,0x754d2c}
 local c=rgb(colors[floor(y/31)%#colors+1]);return {r=c.r-5,g=c.g-5,b=c.b-4}
end
local function mix(a,b,t) return {r=floor(a.r*(1-t)+b.r*t),g=floor(a.g*(1-t)+b.g*t),b=floor(a.b*(1-t)+b.b*t)} end
local function panel(x,y,w,h,c,alpha)
 for yy=y,y+h-1 do rect(x,yy,w,1,mix(wood(yy),c,alpha)) end
end
local function draw_menu()
 D.clear(C.wood)
 for y=0,447 do rect(0,y,368,1,wood(y)) end
 for y=0,447,31 do
  rect(0,y+27,368,3,rgb(0x6a4325))
  for n=0,4 do local x=(n*83+floor(y/31)*47)%368
   rect(x,y+5+(n%3)*5,28+n*3,3,rgb(0x7b512e))
   rect(x+12,y+18,17,2,rgb(0x734b2b))
  end
 end
 border(0,0,368,448,rgb(0x50371f),2)
 local dark=rgb(0x2a2319)
 for i,name in ipairs({'RODS','REELS','LURES'}) do
  local x=18+(i-1)*114
  panel(x,16,106,36,i==tab and rgb(0xe6d1a2) or dark,i==tab and .81 or .80)
  centered(x,27,106,name,i==tab and C.darkwood or C.cream,2)
 end
 local list=tab==1 and rods or (tab==2 and reels or lures)
 for slot=1,9 do
  local i=pages[tab]*9+slot
  local x=32+(slot-1)%3*112;local y=66+floor((slot-1)/3)*96
  panel(x,y,88,88,dark,.70)
  rect(x,y,88,1,rgb(0x664627));rect(x,y+87,88,1,rgb(0x5c3d23))
  local item=list[i]
  if item then
   local enabled=tab==1 or compatible(rod,item,tab==2)
   if tab==1 then
    rod_thumbnail(item,x,y)
    text(x+86-#item.label*12,y+5,item.label,C.cream,2)
   elseif tab==2 then inventory_reel(item.kind,x+44,y+46,not enabled,i==2 and C.red or (i==3 and C.gold or (i==4 and C.cyan or C.gray)))
   else lure_icon(item.icon,x+44,y+41,2.1,not enabled) end
   if i==focus[tab] then border(x-2,y-2,92,92,C.cream,3) end
   local equipped=tab==1 and rod_index or (tab==2 and reel_index or lure_index)
   if i==equipped then rect(x+5,y+5,7,7,C.green);rect(x+6,y+6,3,3,C.white) end
  end
 end
 -- Page controls live in the margins; the three-by-three layout stays intact.
 text(3,204,'<',C.cream,2);text(354,204,'>',C.cream,2)
 centered(100,347,168,tostring(pages[tab]+1)..' / '..math.ceil(#list/9),C.cream,1)
 panel(14,360,340,85,dark,.70)
 local item=list[focus[tab]];if not item then return end
 text(25,365,brands[item.brand],item.brand==1 and C.cyan or C.cream,1.5,true)
 text(25,380,item.name,C.white,1.5)
 text(25,394,item.model,C.cream,1.5)
 if tab==1 then
  if detail_page%2==0 then
   text(25,410,'LENGTH '..item.label..'  POWER '..item.power,C.cream,1.5)
   text(25,427,'ACTION '..item.action..'  WEIGHT '..item.mass,C.cream,1.5)
  else
   text(25,410,item.rating,C.cream,1.5)
   text(25,427,string.upper(item.mount)..' / '..string.upper(item.kind),C.cream,1.5)
  end
 elseif tab==2 then
  text(25,410,item.spec,C.cream,1.5)
  text(25,427,item.capacity,C.cream,1.5)
 else
  text(25,410,'LENGTH '..item.length,C.cream,1.5)
  text(25,427,item.spec,C.cream,1.5)
 end

end
-- Reference landmarks measured after fitting the approved board to 368x448.
local wave_positions={
 {179,224,22},{105,237,26},{329,246,22},{22,256,26},{216,264,23},
 {122,332,29},{29,376,29},{205,384,28},
}
local function sea(t)
 D.clear(C.sky)
 for y=0,202,4 do rect(0,y,368,4,rgb(0x29b6fd)) end
 -- Hand-matched colors and band heights from the approved board.
 rect(0,203,368,95,C.sea1);rect(0,298,368,150,C.sea2)
 for _,c in ipairs({{73,52,50,26},{294,60,47,24},{10,86,37,16}}) do
  rect(c[1],c[2],c[3],15,C.white)
  rect(c[1]+11,c[2]-13,c[4],13,C.white)
 end
 for i,w in ipairs(wave_positions) do
  local x=w[1]+floor(sin(t*.3+i)*1.5);local y=w[2]
  rect(x,y+3,w[3],3,C.foam);rect(x+4,y,w[3]-9,3,C.foam)
  if i%3==0 then rect(x-5,y+2,6,1,C.bluefoam) end
 end
 -- Broad bow and stepped ivory gunwale traced from the reference.
 -- The off-centre apex gives the deck its upper-left heading.
 poly({{288,448},{328,390},{338,382},{348,384},{368,394},{368,448}},rgb(0xf6eed9))
 poly({{298,448},{333,397},{341,390},{350,393},{368,402},{368,448}},rgb(0xcdb793))
 poly({{306,448},{339,401},{345,397},{368,409},{368,448}},rgb(0x806348))
 poly({{316,448},{344,408},{349,405},{368,415},{368,448}},rgb(0xad8960))
 poly({{323,436},{333,423},{368,427},{368,439}},rgb(0xbc976c))
 poly({{338,417},{346,406},{368,417},{368,425}},rgb(0xa17b53))
 poly({{315,448},{322,438},{368,441},{368,448}},rgb(0xc5a073))
 line(332,424,367,429,rgb(0x896641));line(322,437,367,442,rgb(0x94714c))
 line(342,411,365,420,rgb(0x97704b))

end
-- Bending is a distributed curvature along the blank, not a scaled sprite.
-- Power controls compliance; Action moves the curvature onset toward the tip.
local function bend_at(s,power,action,length,load)
 local onset=action_start[action]
 local u=math.max(0,(s-onset)/(1-onset))
 return load/stiffness[power]*(length/6.25)^1.35*u*u
end
local poses={
 idle={{367,447},{339,345},{278,195},{236,123}},
 overhead={{367,447},{315,337},{285,179},{229,97}},
 pendulum={{367,447},{326,321},{293,224},{216,161}},
 iso={{367,447},{338,319},{309,166},{237,86}},
 ['fly-back']={{367,447},{320,352},{275,226},{230,110}},
 ['fly-send']={{367,447},{339,328},{322,196},{271,109}},
 fight={{367,447},{347,270},{300,-65},{196,100}},
}
local function rod_points(r,mode,t,load)
 local p=poses[mode] or poses.idle;local pts={}
 local refpower=mode=='iso' and 'ML' or ((mode=='fly-back' or mode=='fly-send') and 'L' or 'ML')
 local reffeet=mode=='iso' and 10.5 or ((mode=='fly-back' or mode=='fly-send') and 9 or 6.25)
 local length=clamp(1+(r.feet-reffeet)*.045,.85,1.16)
 local response=(stiffness[refpower]/stiffness[r.sim_power or r.power]-1)*14+(action_start.F-action_start[r.sim_action or r.action])*20
 for i=0,64 do
  local u=i/64;local v=1-u
  local x=v^3*p[1][1]+3*v*v*u*p[2][1]+3*v*u*u*p[3][1]+u^3*p[4][1]
  local y=v^3*p[1][2]+3*v*v*u*p[2][2]+3*v*u*u*p[3][2]+u^3*p[4][2]
  local deformation=bend_at(u,'ML',r.sim_action or r.action,6.25,1)
  local bend=(response+(load or 0)*.015)*deformation
  local wobble=(mode=='idle' and .6 or 2)*sin(t*8-8)*u^3
  pts[#pts+1]={367+(x-367)*length-bend+wobble,447+(y-447)*length+bend*.3}
 end
 return pts
end
local function draw_rod(r,reel,mode,t,ghost)
 local lure=r.kind=='iso' and lures[7] or (r.kind=='fly' and lures[8] or (lures[lure_index] or lures[1]))
 local load=(mode=='idle' and 4 or (mode=='fight' and 82*lure.drag or (mode=='overhead' and 45 or 25)))*(.8+lure.weight/40)
 local pts=rod_points(r,mode,t,load)
 if ghost and (mode=='overhead' or mode=='fly-send') then
  for j=1,2 do
   local last=nil
   for i,p in ipairs(pts) do
    local s=(i-1)/64
    local q={p[1]+j*22*s*s,p[2]-j*4*s}
    if last and i>14 then stroke(last[1],last[2],q[1],q[2],2.4,j==1 and C.foam or rgb(0x83c8f1)) end;last=q
   end
  end
 end
 for i=2,#pts do
  local a,b=pts[i-1],pts[i]
  stroke(a[1],a[2],b[1],b[2],i<(r.kind=='fly' and 16 or 14) and 10 or (i<45 and 7 or 4),C.ink)
 end
 for i=2,#pts do
  local a,b=pts[i-1],pts[i]
  local grip=i<(r.kind=='fly' and 16 or 14)
  stroke(a[1],a[2],b[1],b[2],grip and 7 or (i<45 and 4 or 2),grip and (r.kind=='fly' and i>5 and C.cork or C.grip) or C.gold)
 end
 local band=pts[r.kind=='fly' and 16 or 14]
 stroke(band[1]-2,band[2]+1,band[1]+3,band[2]-1,3,C.shine)
 local mount=pts[r.kind=='fly' and 5 or 15]
 local nextp=pts[r.kind=='fly' and 6 or 16]
 -- atan2 handles all quadrants; rod points up/left.
 local angle=math.atan(nextp[2]-mount[2],nextp[1]-mount[1])
 -- Positive normal points to upper/right for a NW rod. Our reel local negative
 -- y is the upper side of a horizontal right-facing rod, so mirror for view.
 local side_angle=angle+pi
 if reel then reel_icon(reel,mount[1],mount[2],1.25,side_angle,false) end
 for _,i in ipairs({20,34,48,59}) do
  local p=pts[i];local q=(reel=='bait' or reel=='round') and 2 or -2
  rect(p[1]+q,p[2]-1,2,3,C.ink)
 end
 return pts[#pts]
end
local function dashed_arc(points,color)
 for i=2,#points do if floor(i/3)%2==0 then stroke(points[i-1][1],points[i-1][2],points[i][1],points[i][2],2,color) end end
end
-- Submerged tackle is a low-contrast silhouette: no eyes, hardware or highlights.
local function submerged_lure(index,x,y)
 local shapes={
  {{-15,0},{-10,-3},{3,-3},{15,0},{6,3},{-10,3}},
  {{-16,0},{-11,-2},{8,-2},{16,0},{8,2},{-12,2}},
  {{-15,0},{-10,-3},{3,-2},{11,0},{14,-4},{17,-4},{17,4},{14,4},{11,1},{2,3},{-10,3}},
  {{-13,0},{-8,-5},{0,-5},{10,0},{5,4},{-8,4},{-16,7}},
  {{-14,-3},{-8,-3},{13,1},{15,3},{-14,3}},
  {{-15,0},{-8,-4},{2,-3},{14,3},{5,2},{-9,3}},
 }
 local points=shapes[index] or {{-12,0},{-5,-2},{11,-5},{5,0},{12,4},{-6,2}}
 local q={};for _,v in ipairs(points) do q[#q+1]={x+v[1]*.85,y+v[2]*.65} end
 poly(q,rgb(0x075da2))
end
local function scene_draw(mode,t,r,reel)
 sea(t)
 local tip=draw_rod(r,reel,mode,t,true)
 if mode=='fly-back' or mode=='fly-send' then
  local old=nil
  for i=0,100 do
   local u=i/100
   local x,y
   if mode=='fly-back' then
    x=200-140*sin(u*2*pi);y=125+47*sin(u*4*pi)
    if i==100 then x,y=54,226 end
   else
    x=tip[1]+u*(32-tip[1]);y=tip[2]+u*80+sin(u*pi*2)*2
   end
   if i==0 then line(tip[1],tip[2],x,y,C.cream) end
   if old then stroke(old[1],old[2],x,y,1.7,C.cream) end;old={x,y}
  end
  lure_icon(8,old[1],old[2],.85,false)
 elseif mode=='fight' then
  local fx,fy=69+sin(t*1.5)*3,351+sin(t*2)*2
  stroke(tip[1],tip[2],fx,fy,1.7,C.white)
  poly({{fx-12,fy+14},{fx-14,fy+2},{fx-4,fy-13},{fx+8,fy-19},{fx+15,fy-13},{fx+13,fy},{fx+2,fy+14}},rgb(0x004778))
  poly({{fx-7,fy+9},{fx-18,fy+17},{fx-14,fy+24},{fx-4,fy+14}},rgb(0x004778))
  poly({{fx-11,fy},{fx-18,fy-3},{fx-17,fy+9}},rgb(0x004778))
  for n=1,14 do local a=n*2.4;local x,y=fx+cos(a)*(20+n),fy+sin(a)*(14+n)
   rect(x,y,4,4,C.white);if n%3==0 then rect(x+1,y-2,2,8,C.white) end
  end
 elseif mode=='idle' then
  local x,y=170,272+sin(t*2)*2
  line(tip[1],tip[2],x,y,C.ink)
  -- A-WA remains visible above the surface; submerged lures only cast a shadow.
  if r.kind=='iso' then lure_icon(7,x,y,.7,false) else
   local bait=lures[lure_index] or lures[r.kind=='fly' and 8 or 1]
   if bait.surface then lure_icon(bait.icon,x,y,.95,false) else submerged_lure(bait.icon,x,y+3) end
  end
  for i=-2,2 do line(x+i*7-5,y+11+math.abs(i)*2,x+i*7+5,y+11+math.abs(i)*2,C.foam) end
 else
  local x,y=mode=='overhead' and 76 or 60,mode=='overhead' and 153 or (mode=='iso' and 185 or 197)
  local points={}
  for i=0,50 do local u=i/50;points[#points+1]={tip[1]+(x-tip[1])*u,tip[2]+(y-tip[2])*u-sin(u*pi)*(mode=='overhead' and 30 or 13)} end
  dashed_arc(points,C.white)
  local prev=points[#points-2]
  local ax,ay=x-prev[1],y-prev[2];local len=math.sqrt(ax*ax+ay*ay)
  if len>0 then poly({{x,y},{x-ax/len*9-ay/len*4,y-ay/len*9+ax/len*4},{x-ax/len*9+ay/len*4,y-ay/len*9-ax/len*4}},C.white) end
  lure_icon(mode=='iso' and 7 or (lures[lure_index] or lures[1]).icon,x-3,y+14,1.15,false,mode=='overhead' and -.85 or -.2)
 end
end
local function test_model()
 for _,r in ipairs(rods) do
  for _,item in ipairs(lures) do assert(compatible(r,item,false)==(r.kind==item.kind)) end
 end
 assert(compatible(rods[1],reels[1],true) and compatible(rods[1],reels[10],true))
 assert(not compatible(rods[1],reels[3],true) and compatible(rods[2],reels[3],true))
 assert(not compatible(rods[1],reels[5],true))
 assert(not compatible(rods[5],reels[1],true) and compatible(rods[5],reels[3],true))
 assert(compatible(rods[7],reels[6],true) and not compatible(rods[7],reels[3],true))
 assert(not compatible(rods[7],reels[5],true))
 assert(bend_at(.7,'UL','R',8,30)>bend_at(.7,'H','R',8,30))
 assert(bend_at(.7,'ML','R',8,30)>bend_at(.7,'ML','XF',8,30))
 assert(bend_at(1,'ML','F',8,30)>bend_at(1,'ML','F',6.25,30))
 for _,r in ipairs(rods) do for _,p in ipairs(powers) do for _,a in ipairs(actions) do
  local rr={feet=r.feet,power=p,action=a}
  for _,m in ipairs(cycle) do
   local pts=rod_points(rr,m,1)
   assert(pts[1][1]==367 and pts[1][2]==447)
   for _,q in ipairs(pts) do assert(q[1]==q[1] and q[2]==q[2] and math.abs(q[1])<736 and math.abs(q[2])<896) end
  end
 end end end
 print('FISHING_CHECK PASS compatibility / bend / corner anchor / 1944 rod poses')
end
local press=nil
local function on_click(x,y,dx,dy,held)
 if menu then
  if dx>60 and math.abs(dx)>math.abs(dy)*1.3 then menu=false;scene='idle';return end
  local list=tab==1 and rods or (tab==2 and reels or lures)
  if math.abs(dy)>60 and math.abs(dy)>math.abs(dx)*1.3 then
   pages[tab]=(pages[tab]+(dy<0 and 1 or -1))%math.ceil(#list/9);return
  end
  if math.abs(dx)>20 or math.abs(dy)>20 then return end
  if y>=66 and y<346 and (x<30 or x>=346) then
   pages[tab]=(pages[tab]+(x<30 and -1 or 1))%math.ceil(#list/9);return
  end
  if y<48 then tab=clamp(floor((x-16)/114)+1,1,3);return end
  if y>=66 and y<346 and x>=32 and x<344 then
   local col=floor((x-32)/112);local row=floor((y-66)/96)
   if (x-32)%112>=88 or (y-66)%96>=88 then return end
   local i=pages[tab]*9+row*3+col+1;local list=tab==1 and rods or (tab==2 and reels or lures)
   if not list[i] then return end
   focus[tab]=i
   if tab==1 then rod_index=i;rod=rods[i];reconcile()
   elseif compatible(rod,list[i],tab==2) then if tab==2 then reel_index=i else lure_index=i end end
   return
  end
  if tab==1 and y>=360 then detail_page=detail_page+1 end
 else
  if dx < -60 and math.abs(dx)>math.abs(dy)*1.3 then menu=true;tab=1;return end
  if math.abs(dx)>20 or math.abs(dy)>20 then return end
  if not reel_index or not lure_index then menu=true;tab=not reel_index and 2 or 3;return end
  scene='idle';cast_start=system.millis()
  -- Temporary mouse gesture rehearsal only, not six-axis sensor recognition.
  A.cast_mode=rod.kind=='fly' and 'fly-back' or (rod.kind=='iso' and 'iso' or (math.abs(dx)>30 and 'pendulum' or 'overhead'))
 end
end
if A.check=='1' then
 test_model()
 local saved={rod_index,reel_index,lure_index,rod,menu,scene,tab,focus,cast_start,pages,detail_page}
 focus={1,1,1};pages={0,0,0};rod_index=1;reel_index=1;lure_index=1;rod=rods[1]
 menu=false
 on_click(70,220,-200,8,250);assert(menu and tab==1)
 on_click(280,220,210,8,250);assert(not menu)
 on_click(350,425,0,0,100);assert(not menu)
 menu=true;tab=1
 on_click(175,194,0,0,100);assert(rod.kind=='iso' and reel_index==nil and lure_index==nil)
 tab=2;on_click(70,90,0,0,100);assert(reel_index==nil)
 on_click(270,90,0,0,100);assert(reel_index==3)
 tab=3;on_click(70,300,0,0,100);assert(lure_index==7)
 tab=1;on_click(70,300,0,0,100);assert(rod.kind=='fly' and reel_index==nil and lure_index==nil)
 tab=2;on_click(175,194,0,0,100);assert(reel_index==nil) -- Hydros III cannot take 8WT
 on_click(290,194,0,0,100);assert(reel_index==6)
 tab=1;on_click(70,90,0,0,100);assert(rod==rods[1])
 local power,action=rod.power,rod.action;on_click(70,420,0,0,100);assert(rod.power==power and rod.action==action)
 tab=2;on_click(358,200,0,0,100);assert(pages[2]==1)
 on_click(70,90,0,0,100);assert(reel_index==10)
 on_click(175,90,0,0,100);assert(reel_index==11)
 tab=3;on_click(180,180,0,-100,100);assert(pages[3]==1)
 on_click(70,90,0,0,100);assert(lure_index==10)
 -- Every catalog cell must be reachable on its page; disabled cells still inspect.
 for tt,list in ipairs({rods,reels,lures}) do
  tab=tt
  for i=1,#list do
   pages[tt]=floor((i-1)/9)
   local slot=(i-1)%9
   on_click(76+(slot%3)*112,100+floor(slot/3)*96,0,0,100)
   assert(focus[tt]==i)
  end
 end
 assert(#rods==9 and #reels==11 and #lures==10)
 for _,list in ipairs({rods,reels,lures}) do for _,v in ipairs(list) do assert(brands[v.brand] and v.name and v.model) end end
 rod_index,reel_index,lure_index,rod,menu,scene,tab,focus,cast_start,pages,detail_page=table.unpack(saved,1,11)
 print('FISHING_INPUT_CHECK PASS paging, immutable specs, rod mounts, fly WT, all catalog items')
end

D.begin_frame({clear=true,color=C.sky})
touch.sync()
print('FISHING_READY 368x448 Lua-only geometry; swipe left opens gear; swipe right returns to sea')
local frames,perf_start=0,system.millis()
while true do
 local now=system.millis()
 local t=fixed and fixed/1000 or (now-start)/1000
 if not fixed then
  local info=touch.poll()
  if info.just_pressed then press={x=info.x,y=info.y,t=now} end
  if info.just_released and press then on_click(info.x,info.y,info.x-press.x,info.y-press.y,now-press.t);press=nil end
 end
 if scene=='cq' then
  D.clear(C.ink)
  centered(0,22,368,'SHIMANO',C.cyan,3)
  centered(0,55,368,'CALCUTTA CONQUEST',C.cream,2)
  inventory_reel('round',165,182,false,C.gold,3)
  centered(0,312,368,'100 RIGHT',C.cream,2)
  centered(0,346,368,'5.6:1  220G  DRAG 4.5KG',C.cream,1.5)
  centered(0,371,368,'NYLON 12LB / 100M',C.cream,1.5)
  centered(0,414,368,'LUA PIXEL GEOMETRY / 3X',C.cyan,1.5)
 elseif menu then draw_menu()
 else
  local mode,r,reel=scene,rod,reels[reel_index] and reels[reel_index].kind or nil
  if scene=='demo' then
   mode=cycle[floor(t/4)%#cycle+1]
   if mode=='iso' then r=rods[5];reel='spin'
   elseif mode=='fly-back' or mode=='fly-send' then r=rods[7];reel='fly' end
  end
  if cast_start then
   local elapsed=(now-cast_start)/1000
   t=elapsed
   if elapsed<2 then mode=A.cast_mode
   elseif elapsed<4 and rod.kind=='fly' then mode='fly-send'
   elseif elapsed<7 then mode='idle' else mode='fight' end
  end
  scene_draw(mode,t,r,reel)
 end
 D.present()
 frames=frames+1
 if now-perf_start>=2000 then print(string.format('FISHING_PERF %.1f FPS',frames*1000/(now-perf_start)));frames=0;perf_start=now end
 delay.delay_ms(math.max(1,33-(system.millis()-now)))
end
