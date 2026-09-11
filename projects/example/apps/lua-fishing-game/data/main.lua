-- AMOLED 368x448 visual prototype. All art is authored geometry drawn in Lua.
-- No image loader, sprite atlas, framebuffer asset, or external font is used.
local D = require('display')
local Physics = require('fishing_math')
local delay = require('delay')
local system = require('system')
local touch = require('lcd_touch')
local A = args or {}
local kernel_times={}
if A.profile=='amoled' then
 local function measured(owner,name)
  local original=owner[name]
  owner[name]=function(...)
   local start=system.millis()
   local a,b=original(...)
   kernel_times[name]=(kernel_times[name] or 0)+system.millis()-start
   return a,b
  end
 end
 for _,name in ipairs({'solve_rope','integrate_rope','damp_rope','rod_pose','rod_step','advance_rope'}) do measured(Physics,name) end
 for _,name in ipairs({'draw_mesh','stroke_path'}) do measured(D,name) end
end
local Budget=A.profile=='amoled' and {fps=60,h=1/120,iterations=6,nodes=72} or {fps=60,h=1/240,iterations=12,nodes=150}
local floor, sin, cos, pi = math.floor, math.sin, math.cos, math.pi
local function clamp(x,a,b) return math.max(a,math.min(b,x)) end
local function rgb(hex) return {r=floor(hex/65536)%256,g=floor(hex/256)%256,b=hex%256} end
local C={sky=rgb(0x29b6fd),sea1=rgb(0x0072d3),sea2=rgb(0x005ebc),sea3=rgb(0x005cba),
 white=rgb(0xf7ffff),foam=rgb(0xbceef5),bluefoam=rgb(0x32b8ed),ink=rgb(0x102532),
 gold=rgb(0xf9b83c),shine=rgb(0xffd976),grip=rgb(0x343c42),gray=rgb(0x809096),
 silver=rgb(0xb8c7c8),red=rgb(0xe85137),cork=rgb(0xd8ac70),green=rgb(0x88dc2d),
 wood=rgb(0x78512e),darkwood=rgb(0x53391f),cream=rgb(0xf3dfad),cyan=rgb(0x91dfec)}
local draw_x,clip_top,clip_bottom=0,0,448
local capture_commands
local frame_draw_calls=0
local function rect(x,y,w,h,c)
 x,y,w,h=floor(x+draw_x+.5),floor(y+.5),floor(w+.5),floor(h+.5)
 local right,bottom=math.min(368,x+w),math.min(clip_bottom,y+h)
 x,y=math.max(0,x),math.max(clip_top,y)
 if right>x and bottom>y then
  if capture_commands then capture_commands[#capture_commands+1]={0,x,y,right-x,bottom-y,c}
  else D.fill_rect(x,y,right-x,bottom-y,c);frame_draw_calls=frame_draw_calls+1 end
 end
end
local function line(x,y,xx,yy,c)
 x,xx=x+draw_x,xx+draw_x
 -- Native API requires in-bounds coordinates. Clip without per-pixel Lua calls.
 local dx,dy=xx-x,yy-y;local lo,hi=0,1
 if dx==0 then if x<0 or x>367 then return end
 else local a,b=-x/dx,(367-x)/dx;if a>b then a,b=b,a end;lo,hi=math.max(lo,a),math.min(hi,b) end
 if dy==0 then if y<clip_top or y>clip_bottom-1 then return end
 else local a,b=(clip_top-y)/dy,(clip_bottom-1-y)/dy;if a>b then a,b=b,a end;lo,hi=math.max(lo,a),math.min(hi,b) end
 if lo>hi then return end
 local ax,ay,bx,by=floor(x+dx*lo+.5),floor(y+dy*lo+.5),floor(x+dx*hi+.5),floor(y+dy*hi+.5)
 if capture_commands then capture_commands[#capture_commands+1]={1,ax,ay,bx,by,c}
 else D.draw_line(ax,ay,bx,by,c);frame_draw_calls=frame_draw_calls+1 end
end
-- Pixel scanline polygon; flat color, no antialiasing, no texture.
local function poly(p,c)
 if D.fill_polygon and not capture_commands then
  D.fill_polygon(p,c,draw_x,clip_top,clip_bottom);frame_draw_calls=frame_draw_calls+1;return
 end
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
 if D.fill_ellipse and not capture_commands then
  D.fill_ellipse(x,y,rx,ry,c,draw_x,clip_top,clip_bottom);frame_draw_calls=frame_draw_calls+1;return
 end
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
local function text(x,y,s,c,scale,bold,scale_y)
 scale=scale or 1
 scale_y=scale_y or scale
 for ch in s:gmatch('.') do
  local rows=glyphs[ch]
  if rows then
   for row=0,6 do local bits=tonumber(rows:sub(row*2+1,row*2+2),16)
    for col=0,4 do if floor(bits/2^(4-col))%2==1 then rect(x+col*scale,y+row*scale_y,scale+(bold and .8 or 0),scale_y,c) end end
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
 {kind='lure',mount='spin',feet=2.62/.3048,label="8'7",power='LML',sim_power='ML',action='N/A',sim_action='RF',brand=2,name='MORETHAN BRANZINO EX AGS',model='87LML',mass='97G',rating='5-30G / PE 0.6-1.5'},
 {kind='iso',mount='spin',feet=5.3/.3048,label='5.3M',power='M',action='DEEP',sim_power='ML',sim_action='R',brand=5,name='MASTER MODEL II KUCHIBUTO',model='M-53',mass='235G',rating='SINKER 1-3 / LEADER 1-2.75'},
 {kind='lure',mount='bait',feet=6.5,label="6'6",power='M',action='RF',brand=2,name='STEEZ',model='C66M TYPE-1.5',mass='93G',rating='5-21G / 8-16LB'},
 {kind='fly',mount='fly',feet=9,label="9'0",power='8WT',wt=8,sim_power='M',action='F',brand=6,name='SALT R8',model='890-4',mass='N/A',rating='8WT / 4 PIECES'},
 {kind='lure',mount='bait',feet=7,label="7'0",power='MH',action='XF',brand=3,name='VERITAS',model='VRPC70-6',mass='N/A',rating='1/4-1OZ / 12-20LB'},
 {kind='lure',mount='bait',feet=7+2/12,label="7'2",power='H',action='XF',brand=4,name='DESTROYER P5 THE X-BITES',model='F5.5-72X',mass='108G',rating='1/4-1OZ / 10-25LB'},
 {kind='lure',mount='bait',feet=6+4/12,label="6'4",power='L',action='FF',sim_action='XF',brand=1,name='POISON ADRENA',model='164L-BFS',mass='82G',rating='3.5-10G / 6-12LB',water='fresh'},
 {kind='lure',mount='bait',feet=6+10/12,label="6'10",power='M',action='RF',brand=2,name='BLAZON',model='C610M-2',mass='110G',rating='5-21G / 8-16LB',water='fresh'},
 {kind='lure',mount='bait',feet=6.75,label="6'9",power='ML',action='F',brand=3,name='ZENON',model='ZENC69-4',mass='N/A',rating='1/8-5/8OZ / 4-10LB',water='fresh'},
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
 {kind='spin',brand=1,name='STRADIC',model='C3000',capacity='PE 1.5 / 270M',spec='5.1:1 225G DRAG 9KG',water='both'},
 {kind='spin',brand=1,name='STRADIC',model='C2000S',capacity='PE 0.6 / 150M',spec='5.1:1 185G DRAG 3KG',water='both'},
 {kind='bait',brand=1,name='SLX',model='70HG',capacity='NYLON 12LB / 100M',spec='7.2:1 195G DRAG 5.5KG',water='fresh'},
 {kind='spin',brand=2,name='24 CERTATE',model='LT4000-C',capacity='PE 1.5 / 200M',spec='5.2:1 235G DRAG 12KG',water='salt'},
 {kind='bait',brand=2,name='24 TATULA TW',model='100H',capacity='NYLON 16LB / 100M',spec='7.1:1 195G DRAG 5KG',water='both'},
 {kind='bait',brand=3,name='REVO LTX',model='BF8',capacity='8LB / 50M',spec='8.0:1 129G DRAG 5.5KG',water='fresh'},
 {kind='bait',brand=3,name='ZENON',model='LTX',capacity='PE 1 / 100M',spec='8.3:1 150G DRAG 5KG',water='both'},
 {kind='spin',brand=3,name='ZENON',model='2500S',capacity='PE 0.8 / 150M',spec='5.2:1 148G DRAG 5KG',water='fresh'},
}
local lures={
 {kind='lure',brand=1,name='SILENT ASSASSIN FB',model='99F / XM-199V',icon=1,weight=14,length='99MM',spec='14G / FLOAT / HOOK 5 X2',drag=.9},
 {kind='lure',brand=2,name='VERTICE R',model='125F-SSR',icon=1,weight=20.3,length='125MM',spec='20.3G / SLOW FLOAT',drag=.9},
 {kind='lure',brand=9,name='FAT SWING IMPACT',model='3.8 IN',icon=3,weight=5,weight_estimated=true,length='96.5MM',spec='WEIGHT N/A / HOOK 2/0',drag=.75},
 {kind='lure',brand=10,name='CHUBBY',model='38F',bill='short',icon=4,weight=4,length='38MM',spec='4G / FLOAT / HOOK 10',drag=1.45},
 {kind='lure',brand=4,name='POP-X',model='POP-X',icon=5,weight=7.087,length='2.5IN',spec='1/4OZ / TOPWATER',surface=true,drag=1.1},
 {kind='lure',brand=11,name='SUPER SPOOK JR.',model='X9236',icon=6,weight=14.175,length='3.5IN',spec='1/2OZ / TOPWATER',surface=true,drag=.65},
 {kind='iso',brand=12,name='A-WA FLOAT RIG',model='PROTOTYPE',icon=7,weight=5,length='N/A',spec='SIMULATION RIG',drag=.5,leader_m=.7},
 {kind='fly',brand=7,name='CLOUSER MINNOW',model='02JX / HOOK 2',icon=8,weight=1,weight_estimated=true,length='N/A',spec='WEIGHT N/A / STREAMER',drag=.4},
 {kind='lure',brand=4,name='VISION ONETEN SW',model='SW',icon=1,weight=14.175,length='4.33IN',spec='1/2OZ / SUSPEND / 4FT',drag=.95},
 {kind='lure',brand=8,name='ORIGINAL FLOATING',model='F07 / US',icon=2,weight=3.544,length='2.75IN',spec='1/8OZ / FLOAT / 3-5FT',drag=.7},
 {kind='lure',brand=1,name='WORLD MINNOW FB',model='115SP / ZQ-K11T',icon=1,weight=17,length='115MM',spec='17G / SUSPEND',drag=.95,water='fresh'},
 {kind='lure',brand=1,name='SILENT ASSASSIN JB',model='129F / XM-129N',icon=1,weight=22,length='129MM',spec='22G / FLOAT',drag=1.05,water='salt'},
 {kind='lure',brand=2,name='STEEZ MINNOW',model='110SP-SR',bill='short',depth_m=1.3,icon=1,weight=14.4,length='110MM',spec='14.4G / SUSPEND',drag=.95,water='fresh'},
 {kind='lure',brand=2,name='SETUPPER',model='125S-DR',bill='long',icon=1,weight=26,length='125MM',spec='26G / SLOW SINK',drag=1.25,water='salt'},
 {kind='lure',brand=2,name='STEEZ MINNOW',model='110SP-MR',bill='medium',depth_m=1.7,icon=1,weight=15,length='110MM',spec='15G / SUSPEND',drag=1.1,water='fresh'},
 {kind='lure',brand=2,name='STEEZ MINNOW',model='110SP-DR',bill='long',depth_m=2.5,icon=1,weight=15.6,length='110MM',spec='15.6G / SUSPEND',drag=1.25,water='fresh'},
 {kind='lure',brand=10,name='CHUBBY',model='38F MR',bill='medium',icon=4,weight=4.2,length='38MM',spec='4.2G / FLOAT',drag=1.6,water='fresh'},
 {kind='lure',brand=10,name='DD CHUBBY',model='38F',bill='long',icon=4,weight=4.2,length='38MM',spec='4.2G / FLOAT',drag=1.75,water='fresh'},
}
-- Initial water-silhouette study; sample catches are preview-only.
local fish_types={
 {name='MARBLED ROCKFISH',coins_per_kg=60,reference_min_kg=0.05,reference_max_kg=0.5,short='ROCKFISH',shape='rock',body=0x997258,back=0x594e48,belly=0xbb9877,fin=0x88654f,mark='mottle',cm=24,kg=.32},
 {name='BLACK SEA BREAM',coins_per_kg=48,reference_min_kg=0.3,reference_max_kg=2,short='BLACK BREAM',shape='bream',body=0x89979a,back=0x455d68,belly=0xc5d3cf,fin=0x445864,mark='bars',cm=38,kg=1.1},
 {name='YELLOWFIN SEABREAM',coins_per_kg=52,reference_min_kg=0.2,reference_max_kg=1.5,short='YELLOWFIN',shape='bream',body=0xa7afa0,back=0x60766c,belly=0xe0dfb5,fin=0xceac48,mark='lines',cm=32,kg=.65},
 {name='LARGESCALE BLACKFISH',coins_per_kg=44,reference_min_kg=0.3,reference_max_kg=1.5,short='BLACKFISH',shape='oval',body=0x4a646d,back=0x293e4c,belly=0x75888b,fin=0x304952,mark='scales',cm=35,kg=.85},
 {name='DUSKY RABBITFISH',coins_per_kg=32,reference_min_kg=0.1,reference_max_kg=0.6,short='RABBITFISH',shape='rabbit',body=0x8b9671,back=0x526c61,belly=0xb6c5a2,fin=0x9aab70,mark='dots',cm=25,kg=.35},
 {name='GRASS PUFFER',coins_per_kg=12,reference_min_kg=0.03,reference_max_kg=0.25,short='PUFFER',shape='puffer',body=0x839476,back=0x4d6554,belly=0xe0dec5,fin=0xafba97,mark='dots',cm=18,kg=.15},
 {name='SPOTTED SEA BASS',coins_per_kg=38,reference_min_kg=0.5,reference_max_kg=4,short='SEA BASS',shape='bass',body=0x98b3b6,back=0x4c7782,belly=0xd1ded6,fin=0x688c95,mark='spots',cm=62,kg=2.4},
 {name='LEOPARD CORAL TROUT',coins_per_kg=180,reference_min_kg=0.5,reference_max_kg=3,short='CORAL TROUT',shape='grouper',body=0xb66b53,back=0x754d53,belly=0xd29a72,fin=0x945a51,mark='blue',cm=48,kg=1.6},
 {name='GREAT BARRACUDA',coins_per_kg=24,reference_min_kg=0.5,reference_max_kg=5,short='BARRACUDA',shape='long',body=0xa7bfc2,back=0x477882,belly=0xd9e2d8,fin=0x678f92,mark='bars',cm=85,kg=3.6},
 {name='SPANISH MACKEREL',coins_per_kg=30,reference_min_kg=1,reference_max_kg=8,short='MACKEREL',shape='torpedo',body=0x8baeb9,back=0x306c82,belly=0xd0dedb,fin=0x52798e,mark='bars',cm=78,kg=3.1},
 {name='GIANT TREVALLY',coins_per_kg=42,reference_min_kg=2,reference_max_kg=15,short='GIANT TREVALLY',shape='trevally',body=0x78919b,back=0x3b596a,belly=0xb2c6c8,fin=0x405d70,mark='scales',cm=70,kg=6.2},
 {name='MAHI-MAHI',coins_per_kg=28,reference_min_kg=2,reference_max_kg=12,short='MAHI-MAHI',shape='mahi',body=0x85b94f,back=0x2b8c91,belly=0xc9ca5b,fin=0x318590,mark='blue',cm=95,kg=5.4},
 {name='BARRAMUNDI',coins_per_kg=46,reference_min_kg=1,reference_max_kg=6,short='BARRAMUNDI',shape='bass',body=0x8aaba6,back=0x426966,belly=0xcbd8c4,fin=0x779a89,mark='scales',cm=70,kg=3.3},
 {name='SILVER SILLAGO',coins_per_kg=56,reference_min_kg=0.05,reference_max_kg=0.25,short='SILLAGO',shape='slender',body=0xb8c6b7,back=0x7d9585,belly=0xe2e5cb,fin=0xa6b894,mark='plain',cm=22,kg=.12},
 {name='FLATHEAD GREY MULLET',coins_per_kg=20,reference_min_kg=0.3,reference_max_kg=1.5,short='GREY MULLET',shape='mullet',body=0x9aafaf,back=0x516c73,belly=0xd1dcd4,fin=0x6c878c,mark='lines',cm=40,kg=.8},
 {name='SPOTTED SCAT',coins_per_kg=26,reference_min_kg=0.1,reference_max_kg=0.5,short='SPOTTED SCAT',shape='scat',body=0xb3a369,back=0x77744f,belly=0xd8cf99,fin=0x8d955f,mark='bigdots',cm=20,kg=.25},
 {name='RED SEA BREAM',coins_per_kg=76,reference_min_kg=0.5,reference_max_kg=3,short='RED SEA BREAM',shape='bream',body=0xc18485,back=0x8f586f,belly=0xe1b9b2,fin=0xa76272,mark='blue',cm=45,kg=1.5},
 {name='MANGROVE JACK',coins_per_kg=68,reference_min_kg=0.3,reference_max_kg=3,short='MANGROVE JACK',shape='snapper',body=0xa46d59,back=0x674d4c,belly=0xcb9e7a,fin=0x925749,mark='scales',cm=42,kg=1.3},
 {name='RED EMPEROR',coins_per_kg=88,reference_min_kg=0.5,reference_max_kg=4,short='RED EMPEROR',shape='bream',body=0xd9bb9d,back=0xb9907f,belly=0xe8d6b8,fin=0xa75455,mark='redbands',cm=35,kg=.9},
 {name='ORANGE-SPOTTED GROUPER',coins_per_kg=96,reference_min_kg=0.5,reference_max_kg=4,short='ORANGE-SPOT',shape='grouper',body=0xa99c74,back=0x6b735e,belly=0xd0c09a,fin=0x878467,mark='orange',cm=48,kg=1.8},
 {name='BROWN-MARBLED GROUPER',coins_per_kg=120,reference_min_kg=1,reference_max_kg=6,short='TIGER GROUPER',shape='grouper',body=0x9b946e,back=0x565b46,belly=0xc6b990,fin=0x787a54,mark='mottle',cm=55,kg=2.8},
 {name='GIANT GROUPER',coins_per_kg=110,reference_min_kg=5,reference_max_kg=40,short='GIANT GROUPER',shape='giant',body=0x74756a,back=0x454f4b,belly=0x9c9e87,fin=0x5c6858,mark='mottle',cm=130,kg=38},
 {name='HONG KONG GROUPER',coins_per_kg=140,reference_min_kg=0.2,reference_max_kg=1.5,short='RED-SPOT GROUPER',shape='grouper',body=0xb99a81,back=0x806d66,belly=0xd3bfa1,fin=0x9c795e,mark='orange',cm=32,kg=.65},
 {name='KELP GROUPER',coins_per_kg=100,reference_min_kg=1,reference_max_kg=6,short='KELP GROUPER',shape='grouper',body=0x857d63,back=0x494e3d,belly=0xaca38b,fin=0x68634d,mark='cloud',cm=58,kg=3.3},
 {name='MACKEREL SCAD',coins_per_kg=10,reference_min_kg=0.08,reference_max_kg=0.35,short='MACKEREL SCAD',shape='slender',body=0x8fb8bc,back=0x387f8b,belly=0xd2e2d9,fin=0x639c9a,mark='plain',cm=24,kg=.18},
 {name='BLUEFIN TREVALLY',coins_per_kg=50,reference_min_kg=1,reference_max_kg=5,short='BLUEFIN TREVALLY',shape='trevally',body=0x84a78d,back=0x47796f,belly=0xc0ceaa,fin=0x338ecc,mark='blue',cm=55,kg=2.6},
 {name='GREATER AMBERJACK',coins_per_kg=36,reference_min_kg=2,reference_max_kg=15,short='AMBERJACK',shape='torpedo',body=0x94a996,back=0x517a77,belly=0xc9d6bd,fin=0x829760,mark='goldstripe',cm=80,kg=5.5},
 {name='LARGEHEAD HAIRTAIL',coins_per_kg=40,reference_min_kg=0.2,reference_max_kg=1.2,short='HAIRTAIL',shape='ribbon',body=0xb4cbd0,back=0x7298ae,belly=0xe0e8df,fin=0x8aa8b3,mark='plain',cm=90,kg=.8},
 {name='YELLOWFIN TUNA',coins_per_kg=80,reference_min_kg=5,reference_max_kg=40,short='YELLOWFIN TUNA',shape='tuna',body=0x799cab,back=0x2c576f,belly=0xc4d5d5,fin=0xd6bb4b,mark='plain',cm=110,kg=22},
 {name='SKIPJACK TUNA',coins_per_kg=22,reference_min_kg=1,reference_max_kg=5,short='SKIPJACK',shape='tuna',body=0x7a9ca4,back=0x354f66,belly=0xbcd0cb,fin=0x4d7480,mark='bellylines',cm=55,kg=3.2},
}
-- First-pass behavior calibration, not measured biological constants.
-- Zero sprint_min_kg explicitly disables sustained runs for small species.
local fish_behavior_profiles={
 -- MARBLED ROCKFISH
 {sprint_min_kg=0,burst=22,endurance=18,agility=35,headshake=80,dive=65,leap=0,cover=95,recovery=40,caution=30,hook_hold=65,sprint_seconds=0,sprint_cooldown=5},
 -- BLACK SEA BREAM
 {sprint_min_kg=1.2,burst=48,endurance=55,agility=62,headshake=55,dive=60,leap=0,cover=55,recovery=45,caution=75,hook_hold=70,sprint_seconds=1.8,sprint_cooldown=6},
 -- YELLOWFIN SEABREAM
 {sprint_min_kg=1,burst=45,endurance=48,agility=68,headshake=50,dive=55,leap=0,cover=45,recovery=48,caution=72,hook_hold=65,sprint_seconds=1.5,sprint_cooldown=6},
 -- LARGESCALE BLACKFISH
 {sprint_min_kg=1,burst=55,endurance=65,agility=68,headshake=50,dive=65,leap=0,cover=65,recovery=48,caution=85,hook_hold=65,sprint_seconds=1.8,sprint_cooldown=7},
 -- DUSKY RABBITFISH
 {sprint_min_kg=0,burst=35,endurance=40,agility=72,headshake=45,dive=40,leap=0,cover=65,recovery=50,caution=65,hook_hold=55,sprint_seconds=0,sprint_cooldown=5},
 -- GRASS PUFFER
 {sprint_min_kg=0,burst=12,endurance=15,agility=25,headshake=25,dive=20,leap=0,cover=30,recovery=35,caution=25,hook_hold=75,sprint_seconds=0,sprint_cooldown=5},
 -- SPOTTED SEA BASS
 {sprint_min_kg=2,burst=72,endurance=50,agility=85,headshake=85,dive=30,leap=55,cover=55,recovery=58,caution=70,hook_hold=55,sprint_seconds=2.5,sprint_cooldown=6},
 -- LEOPARD CORAL TROUT
 {sprint_min_kg=1.5,burst=76,endurance=48,agility=45,headshake=65,dive=95,leap=0,cover=98,recovery=35,caution=60,hook_hold=78,sprint_seconds=1.4,sprint_cooldown=8},
 -- GREAT BARRACUDA
 {sprint_min_kg=2,burst=92,endurance=55,agility=65,headshake=60,dive=15,leap=20,cover=10,recovery=50,caution=60,hook_hold=62,sprint_seconds=3.5,sprint_cooldown=7},
 -- SPANISH MACKEREL
 {sprint_min_kg=3,burst=95,endurance=75,agility=60,headshake=45,dive=35,leap=10,cover=5,recovery=55,caution=45,hook_hold=55,sprint_seconds=4.2,sprint_cooldown=7},
 -- GIANT TREVALLY
 {sprint_min_kg=5,burst=98,endurance=92,agility=75,headshake=50,dive=55,leap=10,cover=35,recovery=55,caution=55,hook_hold=85,sprint_seconds=5.5,sprint_cooldown=9},
 -- MAHI-MAHI
 {sprint_min_kg=4,burst=85,endurance=70,agility=90,headshake=75,dive=15,leap=95,cover=0,recovery=62,caution=40,hook_hold=60,sprint_seconds=3.6,sprint_cooldown=6},
 -- BARRAMUNDI
 {sprint_min_kg=3,burst=78,endurance=65,agility=75,headshake=82,dive=45,leap=45,cover=65,recovery=52,caution=65,hook_hold=65,sprint_seconds=2.8,sprint_cooldown=7},
 -- SILVER SILLAGO
 {sprint_min_kg=0,burst=18,endurance=18,agility=60,headshake=45,dive=15,leap=0,cover=10,recovery=55,caution=60,hook_hold=30,sprint_seconds=0,sprint_cooldown=5},
 -- FLATHEAD GREY MULLET
 {sprint_min_kg=1,burst=65,endurance=70,agility=80,headshake=40,dive=20,leap=45,cover=5,recovery=60,caution=85,hook_hold=35,sprint_seconds=2.0,sprint_cooldown=6},
 -- SPOTTED SCAT
 {sprint_min_kg=0,burst=25,endurance=35,agility=70,headshake=45,dive=40,leap=0,cover=60,recovery=50,caution=60,hook_hold=45,sprint_seconds=0,sprint_cooldown=5},
 -- RED SEA BREAM
 {sprint_min_kg=2,burst=65,endurance=65,agility=60,headshake=70,dive=70,leap=0,cover=40,recovery=45,caution=75,hook_hold=75,sprint_seconds=2.3,sprint_cooldown=7},
 -- MANGROVE JACK
 {sprint_min_kg=1.5,burst=82,endurance=58,agility=80,headshake=65,dive=75,leap=15,cover=90,recovery=45,caution=70,hook_hold=80,sprint_seconds=2.2,sprint_cooldown=7},
 -- RED EMPEROR
 {sprint_min_kg=2,burst=70,endurance=70,agility=55,headshake=60,dive=75,leap=0,cover=60,recovery=40,caution=60,hook_hold=75,sprint_seconds=2.4,sprint_cooldown=7},
 -- ORANGE-SPOTTED GROUPER
 {sprint_min_kg=2,burst=72,endurance=50,agility=38,headshake=62,dive=92,leap=0,cover=95,recovery=35,caution=55,hook_hold=75,sprint_seconds=1.5,sprint_cooldown=8},
 -- BROWN-MARBLED GROUPER
 {sprint_min_kg=3,burst=78,endurance=58,agility=35,headshake=70,dive=98,leap=0,cover=98,recovery=32,caution=60,hook_hold=85,sprint_seconds=1.6,sprint_cooldown=9},
 -- GIANT GROUPER
 {sprint_min_kg=10,burst=95,endurance=80,agility=22,headshake=85,dive=100,leap=0,cover=100,recovery=25,caution=45,hook_hold=95,sprint_seconds=2.3,sprint_cooldown=12},
 -- HONG KONG GROUPER
 {sprint_min_kg=1,burst=65,endurance=42,agility=45,headshake=62,dive=88,leap=0,cover=95,recovery=38,caution=65,hook_hold=70,sprint_seconds=1.2,sprint_cooldown=7},
 -- KELP GROUPER
 {sprint_min_kg=3,burst=80,endurance=60,agility=42,headshake=70,dive=96,leap=0,cover=98,recovery=32,caution=75,hook_hold=85,sprint_seconds=1.8,sprint_cooldown=9},
 -- MACKEREL SCAD
 {sprint_min_kg=0,burst=40,endurance=35,agility=80,headshake=35,dive=25,leap=0,cover=0,recovery=65,caution=40,hook_hold=35,sprint_seconds=0,sprint_cooldown=5},
 -- BLUEFIN TREVALLY
 {sprint_min_kg=2.5,burst=88,endurance=80,agility=90,headshake=55,dive=45,leap=10,cover=25,recovery=58,caution=75,hook_hold=75,sprint_seconds=4.0,sprint_cooldown=7},
 -- GREATER AMBERJACK
 {sprint_min_kg=5,burst=92,endurance=90,agility=60,headshake=45,dive=90,leap=0,cover=30,recovery=50,caution=55,hook_hold=80,sprint_seconds=4.8,sprint_cooldown=9},
 -- LARGEHEAD HAIRTAIL
 {sprint_min_kg=0.8,burst=60,endurance=35,agility=75,headshake=78,dive=45,leap=0,cover=5,recovery=58,caution=50,hook_hold=40,sprint_seconds=1.5,sprint_cooldown=6},
 -- YELLOWFIN TUNA
 {sprint_min_kg=10,burst=96,endurance=100,agility=58,headshake=35,dive=82,leap=5,cover=0,recovery=55,caution=50,hook_hold=90,sprint_seconds=6.5,sprint_cooldown=10},
 -- SKIPJACK TUNA
 {sprint_min_kg=3,burst=85,endurance=85,agility=72,headshake=45,dive=60,leap=5,cover=0,recovery=65,caution=45,hook_hold=70,sprint_seconds=3.8,sprint_cooldown=7},
}
for i,f in ipairs(fish_types) do f.behavior=fish_behavior_profiles[i] end
-- Pure probability policy for later live-AI integration. No frame-based RNG.
local function fish_behavior_weights(f,kg,energy,near_cover,near_surface,cooldown,last_action)
 local b=f.behavior
 energy=clamp(energy,0,1)
 local can_run=b.sprint_min_kg>0 and kg>=b.sprint_min_kg and energy>.35 and (cooldown or 0)<=0
 local weights={
  run=can_run and b.burst*b.endurance/100*energy or 0,
  turn=b.agility*.50,
  shake=b.headshake*.45,
  dive=b.dive*.35*(near_cover and (1+b.cover/100) or .45),
  jump=near_surface and b.leap*.30*energy or 0,
  recover=12+(1-energy)*b.recovery,
 }
 if last_action and weights[last_action] then weights[last_action]=weights[last_action]*.5 end
 local total=0;for _,v in pairs(weights) do total=total+v end
 for action,v in pairs(weights) do weights[action]=v/total end
 return weights
end
-- Fictional game economy, denominated in coins/kg, not retail seafood prices.
local function fish_value(f) return math.floor(f.kg*f.coins_per_kg+.5) end
local catches={}
local bag_preview=A.scene=='fish-bag-preview'
local powers={'UL','L','ML','M','MH','H','XH','XXH','XXXH'}
local actions={'R','RF','F','XF'}
local stiffness={UL=.55,L=.75,ML=1,M=1.25,MH=1.6,H=2,XH=2.5,XXH=3.1,XXXH=3.8}
local action_start={R=.12,RF=.28,F=.48,XF=.64}
-- Mount describes the reel seat; discipline controls the permitted rig.
local function compatible(rod,item,is_reel)
 if not rod or not item then return false end
 if is_reel then
  if rod.kind=='fly' then
   return rod.mount=='fly' and item.kind=='fly' and rod.wt~=nil and
    item.wtmin~=nil and item.wtmax~=nil and rod.wt>=item.wtmin and rod.wt<=item.wtmax
  end
  if rod.mount=='spin' then return item.kind=='spin' end
  if rod.mount=='bait' and rod.kind=='lure' then return item.kind=='bait' or item.kind=='round' end
  return false
 end
 if rod.kind=='fly' then return item.kind=='fly' end
 if rod.kind=='iso' then return rod.water~='fresh' and item.kind=='iso' end
 return rod.kind=='lure' and item.kind=='lure'
end
local function rod_mark(r)
 return r.kind=='fly' and 'F' or (r.mount=='bait' and 'C' or 'S')
end
local function rod_length(r)
 local inches=floor(r.feet*12+.5)
 return string.format("%d'%d\"",floor(inches/12),inches%12)
end
local rod_index=tonumber(A.rod) or 1
local reel_index=tonumber(A.reel) or 1
local lure_index=tonumber(A.lure) or 1
local rod=rods[rod_index]
-- Real catalog specs are immutable; CLI visual overrides are intentionally unused.
local function reconcile()
 if not compatible(rod,reels[reel_index],true) then reel_index=nil end
 if not compatible(rod,lures[lure_index],false) then lure_index=nil end
end
reconcile()
local scene=A.scene or 'idle'
if scene=='iso' then rod_index=4;rod=rods[4];reel_index=4;lure_index=7
elseif scene=='fly-back' or scene=='fly-send' then rod_index=6;rod=rods[6];reel_index=6;lure_index=8 end
local menu=scene=='rods' or scene=='reels' or scene=='lures' or scene=='fish-bag' or bag_preview
local tab=(scene=='fish-bag' or bag_preview) and 4 or (scene=='reels' and 2 or (scene=='lures' and 3 or 1))
local focus={rod_index,reel_index or 1,lure_index or 1,1}
local function catalog(t) return t==1 and rods or (t==2 and reels or (t==3 and lures or (bag_preview and fish_types or catches))) end
local function max_scroll(t) return math.max(0,math.ceil(#catalog(t)/3)*96-8-284) end
local scrolls={}
for i=1,4 do scrolls[i]=clamp(floor((focus[i]-1)/3)*96,0,max_scroll(i)) end
local press,slide=nil,nil
local function switch_tab(target,offset)
 target=clamp(target,1,4)
 if target~=tab or (offset or 0)~=0 then
  slide={from=tab,to=target,x=offset or 0,start=system.millis()}
 end
 tab=target
end
local detail_page=tonumber(A.detail) or 0
local fixed=tonumber(A.time_ms)
local start=system.millis()
local cast_start=nil
local cycle={'overhead','pendulum','iso','fly-back','fly-send','fight'}
local function wordmark(value,x,y,color,sx,style)
 for i=1,#value do
  local rows=glyphs[value:sub(i,i)]
  if rows then
   for row=0,6 do
    local bits=tonumber(rows:sub(row*2+1,row*2+2),16)
    local lean=style=='italic' and floor((6-row)*.65) or 0
    for col=0,4 do if floor(bits/2^(4-col))%2==1 then
     rect(x+(i-1)*6*sx+col*sx+lean,y+row*2.5,sx+.65,2.5,color)
    end end
   end
  end
 end
end
local function logo(brand,x,y)
 if brand==1 then text(x,y,'SHIMANO',C.cyan,2.5,true)
 elseif brand==2 then
  poly({{x,y},{x+19,y},{x+27,y+9},{x+16,y+19},{x+1,y+19},{x+14,y+8},{x,y+8}},C.white)
  poly({{x+24,y},{x+31,y},{x+41,y+9},{x+29,y+19},{x+20,y+19},{x+32,y+9}},C.white)
  text(x+48,y,'DAIWA',C.white,2.5,true)
 elseif brand==3 then
  rect(x,y,23,8,C.red);rect(x-2,y+12,25,6,C.red)
  -- Match the 2.5x letter height of SHIMANO/DAIWA within the same column.
  text(x+29,y,'ABU Garcia',C.white,1.5,true,2.5)
 elseif brand==4 then
  wordmark('Megabass',x,y,C.white,2.25,'italic')
  stroke(x+7,y+21,x+108,y+17,1,C.red)
 elseif brand==5 then
  wordmark('Gamakatsu',x,y,C.white,2,'italic')
 elseif brand==6 then
  wordmark('SAGE',x,y,C.cream,3,'italic')
 elseif brand==7 then
  wordmark('ORVIS',x,y,C.white,3,'serif')
  for _,dx in ipairs({0,36,54,72}) do rect(x+dx,y,6,2,C.white);rect(x+dx,y+16,6,2,C.white) end
 elseif brand==8 then
  wordmark('RAPALA',x,y,C.red,3,'italic')
 elseif brand==9 then
  text(x-1,y-1,'KEITECH',C.gold,2.5,true)
  text(x+1,y+1,'KEITECH',C.gold,2.5,true)
  text(x,y,'KEITECH',C.ink,2.5,true)
 elseif brand==10 then
  -- JACKALL's angular jackal head, in its square emblem.
  border(x,y-2,25,25,C.white,2)
  poly({{x+4,y+1},{x+11,y+6},{x+20,y+1},{x+19,y+11},{x+13,y+19},{x+6,y+12}},C.white)
  poly({{x+6,y+4},{x+10,y+8},{x+7,y+9}},C.ink)
  poly({{x+18,y+4},{x+15,y+8},{x+18,y+9}},C.ink)
  line(x+8,y+11,x+11,y+12,C.ink);line(x+17,y+11,x+14,y+12,C.ink)
  rect(x+12,y+17,3,2,C.ink)
  wordmark('JACKALL',x+31,y,C.white,2,'italic')
 elseif brand==11 then
  wordmark('Heddon',x,y,C.red,3,'italic')
  stroke(x+6,y+21,x+102,y+18,1,C.red)
 else
  border(x,y,18,18,C.cream,2);text(x+6,y+5,'C',C.cream,1)
  wordmark('CUSTOM',x+25,y,C.cream,2,'plain')
 end
end
-- Each catalog reel owns a geometry profile. Dimensions are pixel-art design
-- units, not manufacturer measurements. X points toward the butt, Y is the
-- spool axle across the rod, Z is above the rod. One orthographic camera sees
-- the top and near side; never rotate a face-on icon to fake that view.
local reel_profiles={
 {body=0xaeb8be,trim=0xdce6e8,len=8.4,wide=5.8,height=4.8,nose=2.5,handle=7.5}, -- ANTARES
 {body=0x30343b,trim=0xb93932,len=7.8,wide=5.4,height=4.7,nose=1.8,handle=8,left=true}, -- SX LEFT
 {body=0xa9b1af,trim=0xc4ab65,len=6.5,wide=4.2,height=10,spool=3.5}, -- STELLA
 {body=0x929fa5,trim=0xc5b887,len=6.7,wide=4.3,height=10.3,spool=3.5}, -- TWINPOWER
 {body=0x59636a,trim=0xc4ced0,radius=6.0,wide=2.4,spokes=6}, -- HYDROS III
 {body=0x59636a,trim=0xc4ced0,radius=6.57,wide=2.6,spokes=6}, -- HYDROS IV
 {body=0xc2cdcf,trim=0xe4e5d8,len=5.6,wide=3.7,height=9.2,spool=3.1}, -- EXIST
 {body=0x40494e,trim=0xad9c65,len=5.9,wide=3.8,height=9.4,spool=3.1}, -- LUVIAS
 {body=0x444c50,trim=0x8b9496,len=7.0,wide=5.0,height=4.3,nose=2.0,handle=7.5,twing=true}, -- STEEZ
 {body=0xc89d4c,trim=0xf6d37c,radius=4.7,wide=5.6,handle=7.2}, -- CQ 100
 {body=0xc89d4c,trim=0xf6d37c,radius=5.2,wide=6.1,handle=7.7}, -- CQ 200
 {body=0xb6c0c5,trim=0xe2e4de,len=6.5,wide=4.2,height=10,spool=3.5}, -- STRADIC C3000
 {body=0xb6c0c5,trim=0xe2e4de,len=5.6,wide=3.7,height=9,spool=2.9}, -- STRADIC C2000S
 {body=0x323c48,trim=0x3b7bc0,len=7.3,wide=5.1,height=4.4,nose=1.4,handle=7.2}, -- SLX
 {body=0x626c70,trim=0xc6b678,len=7.0,wide=4.6,height=11,spool=3.9}, -- CERTATE
 {body=0x333b42,trim=0xb8bab2,len=8.0,wide=5.6,height=4.8,nose=1.6,handle=8,twing=true}, -- TATULA
 {body=0x393e43,trim=0xb7c1c4,len=6.7,wide=4.8,height=4.0,nose=1.8,handle=7.5,port=true}, -- BF8
 {body=0x8f9caa,trim=0xd0dbe0,len=6.6,wide=4.7,height=4.1,nose=2.3,handle=7.5,port=true}, -- ZENON LTX
 {body=0x737d88,trim=0xc0cbd3,len=5.3,wide=3.5,height=9.0,spool=3.1}, -- ZENON 2500S
}
local reel_meshes={}
local reel_mesh_handles={}
local function build_reel_mesh(item,g)
 local faces={}
 local function face(v,color)
  local a,b,c=v[1],v[2],v[3]
  local ux,uy,uz=b[1]-a[1],b[2]-a[2],b[3]-a[3]
  local vx,vy,vz=c[1]-a[1],c[2]-a[2],c[3]-a[3]
  local nx,ny,nz=uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx
  if nx*.342-ny*.664+nz*.664<=0 then return end
  local points,depth={},0
  for _,p in ipairs(v) do
   points[#points+1]={.93969*p[1]+.241845*p[2]-.241845*p[3],-.707107*(p[2]+p[3])}
   depth=depth+.342*p[1]-.664*p[2]+.664*p[3]
  end
  local norm=math.sqrt(nx*nx+ny*ny+nz*nz)
  -- Dark metal midtones, deep unlit faces and a narrow bright highlight.
  -- Preserve each model's silver/gold finish rather than tinting it black.
  local lit=clamp((nx*.15-ny*.45+nz*.88)/norm,0,1)
  local shade=.26+.62*lit^1.35+.10*lit^16
  local k=rgb(color)
  faces[#faces+1]={p=points,z=depth/#v,c={r=floor(k.r*shade),g=floor(k.g*shade),b=floor(k.b*shade)}}
 end
 local function block(x,y,z,l,w,h,color)
  local a,b,c,d={x,y,z},{x+l,y,z},{x+l,y+w,z},{x,y+w,z}
  local e,f,g,hp={x,y,z+h},{x+l,y,z+h},{x+l,y+w,z+h},{x,y+w,z+h}
  face({d,c,b,a},color);face({e,f,g,hp},color)
  face({a,b,f,e},color);face({b,c,g,f},color);face({c,d,hp,g},color);face({d,a,e,hp},color)
 end
 local function cylinder(axis,x,y,z,r,length,color,cap)
  local back,front={},{}
  for i=0,15 do
   local a=i*pi/8;local co,si=cos(a)*r,sin(a)*r
   if axis=='y' then back[i+1]={x+co,y,z+si};front[i+1]={x+co,y+length,z+si}
   else back[i+1]={x,y+co,z+si};front[i+1]={x+length,y+co,z+si} end
  end
  -- X ring orientation is +X, Y ring orientation is -Y.
  local reverse={};for i=16,1,-1 do reverse[#reverse+1]=back[i] end
  local revfront={};for i=16,1,-1 do revfront[#revfront+1]=front[i] end
  if axis=='y' then face(back,cap or color);face(revfront,cap or color)
  else face(reverse,cap or color);face(front,cap or color) end
  for i=1,16 do local j=i%16+1
   if axis=='y' then face({back[i],front[i],front[j],back[j]},color)
   else face({back[i],back[j],front[j],front[i]},color) end
  end
 end
 -- Rounded, beveled casting shell and gear housing, built as nested rings.
 local function shell(outline,width,color)
  local cx,cz=0,0
  for _,p in ipairs(outline) do cx=cx+p[1];cz=cz+p[2] end
  cx,cz=cx/#outline,cz/#outline
  local rings={}
  for _,t in ipairs({{-1,.77},{-.84,.95},{-.5,1},{.5,1},{.84,.95},{1,.77}}) do
   local ring={}
   for _,p in ipairs(outline) do ring[#ring+1]={cx+(p[1]-cx)*t[2],t[1]*width/2,cz+(p[2]-cz)*t[2]} end
   rings[#rings+1]=ring
  end
  local near={};for i=#outline,1,-1 do near[#near+1]=rings[1][i] end
  face(near,color);face(rings[#rings],color)
  for r=1,#rings-1 do
   for i=1,#outline do local j=i%#outline+1
    face({rings[r][i],rings[r][j],rings[r+1][j],rings[r+1][i]},color)
   end
  end
 end
 local function star(x,y,z,r,color)
  local points={}
  for i=0,9 do local a=i*pi/5;local rr=i%2==0 and r or r*.45
   points[#points+1]={x+cos(a)*rr,y,z+sin(a)*rr}
  end
  face(points,color);local back={};for i=#points,1,-1 do back[#back+1]=points[i] end;face(back,color)
 end
 local ink=0x18232b
 block(-3,-.8,-.4,6,1.6,.8,0x727b80) -- foot stays on the rod axis
 if item.kind=='bait' or item.kind=='round' then
  block(-1,-.8,0,2,1.6,2,g.body)
  local w=g.wide;local z=item.kind=='round' and g.radius+1.5 or g.height*.65+1.5
  if item.kind=='round' then
   cylinder('y',0,-w/2,z,g.radius,.8,g.body,g.trim)
   cylinder('y',0,w/2-.8,z,g.radius,.8,g.body,g.trim)
   cylinder('y',0,-w/2+.8,z,g.radius*.64,w-1.6,0x646668)
   block(-g.radius*.75,-w/2,z+g.radius*.44,.7,w,.6,g.trim)
   block(-g.radius*.6,-w/2,z-g.radius*.65,1,w,.7,g.body)
   cylinder('y',0,-w/2-.3,z,g.radius*.88,.3,g.trim,g.body)
   cylinder('y',0,-w/2-.42,z,g.radius*.72,.12,g.body,g.body)
   cylinder('y',0,-w/2-.55,z,1.15,.13,g.trim)
   for i=0,5 do local a=i*pi/3
    cylinder('y',cos(a)*g.radius*.79,-w/2-.48,z+sin(a)*g.radius*.79,.22,.12,0x7a6238)
   end
   for j=-1,1 do cylinder('y',0,j*w*.18-.08,z,g.radius*.65,.12,0x97a6a0) end
  else
   -- Tapered hood, two side plates and a transverse open spool.
   local l,h=g.len,g.height
   shell({{-l*.40,1.4},{-l*.53,1.8},{-l*.50-g.nose*.5,2.8},
    {-l*.45-g.nose*.45,3.6},{-l*.36,h+.6},{-l*.15,h+1.45},
    {l*.10,h+1.55},{l*.32,h+1.1},{l*.46,h*.75+1.2},
    {l*.50,2.8},{l*.40,1.7},{l*.22,1.4}},w,g.body)
   -- Side-cover rim, tension cap and fasteners are separate raised pieces.
   cylinder('y',l*.06,-w/2-.10,3.4,h*.30,.22,g.trim,g.body)
   cylinder('y',l*.06,-w/2-.24,3.4,h*.24,.14,g.body)
   for _,q in ipairs({{-l*.24,2.5},{l*.24,4.0}}) do
    cylinder('y',q[1],-w/2-.28,q[2],.23,.13,g.trim)
   end
   block(-1.8,-w*.30,h+1.35,3.0,w*.60,.25,ink)
   cylinder('y',-.3,-w*.27,h+.75,1.05,w*.54,0x6c777b)
   cylinder('y',-.3,-w*.28,h+.75,1.13,.20,g.trim)
   cylinder('y',-.3,w*.28-.20,h+.75,1.13,.20,g.trim)
   for j=-1,1 do cylinder('y',-.3,j*w*.13-.07,h+.75,1.08,.10,0xc4cdcc) end
   block(l*.28,-w*.3,2.7,1.1,w*.6,.6,ink) -- thumb bar aft of spool
   block(-l*.5-.6,-(g.twing and 1.6 or .8),2.8,.4,g.twing and 3.2 or 1.6,.7,g.trim)
   cylinder('y',1,-w/2-.2,3,1.15,.2,g.trim)
   if g.port then cylinder('y',1,-w/2-.35,3,.6,.1,ink) end
  end
  -- Crank axle is transverse, star drag is behind the double crank.
  local side=g.left and 1 or -1;local hy=side*(w/2+1.5)
  cylinder('y',1,math.min(hy,-w/2),z-1,1.0,math.abs(hy+w/2)+.5,g.trim)
  star(1,hy-side*.45,z-1,2.1,g.trim)
  block(.65,hy-.3,z-1-g.handle/2,.7,.6,g.handle,g.trim)
  cylinder('y',1,hy-.5,z-1,.8,.7,0xd0d7d8)
  cylinder('y',1,hy-.62,z-1,.36,.12,ink)
  for _,sign in ipairs({-1,1}) do
   cylinder('y',1,hy-.9,z-1+sign*g.handle/2,1.0,1.8,0x292f34)
   cylinder('y',1,hy-1.05,z-1+sign*g.handle/2,.70,.15,0x485055)
  end
 elseif item.kind=='spin' then
  local z=-g.height
  block(-.6,-.65,z+2,1.2,1.3,-z-2,g.body) -- stem below reel foot
  shell({{-1.7,z-1.4},{-1.8,z+.8},{-.9,z+2.3},{.8,z+2.6},
   {2.5,z+1.6},{3.3,z-.5},{2.7,z-2.0},{1.0,z-2.6},{-.7,z-2.2}},g.wide,g.body)
  cylinder('y',1,-g.wide/2-.15,z,1.7,.15,g.trim,g.body)
  cylinder('y',1,-g.wide/2-.27,z,1.35,.12,g.body)
  cylinder('y',2,-g.wide/2-.30,z+1.6,.23,.15,g.trim)
  -- Fixed spool axis is parallel to rod, front lip points toward rod tip.
  cylinder('x',-g.len,-.2,z,g.spool*.90,2.7,0x8e9f9e,g.trim)
  for i=0,3 do cylinder('x',-g.len+.35+i*.5,-.2,z,g.spool*.92,.12,0xc6d0c9) end
  cylinder('x',-g.len+2.45,-.2,z,g.spool+.12,.30,g.body,g.trim)
  cylinder('x',-g.len-.3,-.2,z,g.spool+.2,.5,g.trim)
  cylinder('x',-g.len-.65,-.2,z,1.3,.35,ink)
  cylinder('x',-g.len-.85,-.2,z,.7,.20,g.trim)
  cylinder('x',-g.len+2.8,-.2,z,1.3,2.5,g.body)
  -- Bail arcs around the front spool, with rotor arms behind it.
  block(-g.len+2.5,-g.spool-.6,z-.35,2,.6,.7,g.body)
  block(-g.len+2.5,g.spool-.2,z-.35,2,.6,.7,g.body)
  for i=0,11 do
   local a,b=i*pi/12,(i+1)*pi/12
   local function q(t,r) return {-g.len-.6,-.2+cos(t)*r,z+sin(t)*r} end
   local points={q(a,g.spool+.35),q(b,g.spool+.35),q(b,g.spool+.85),q(a,g.spool+.85)}
   face(points,0xd7e2e3);face({points[4],points[3],points[2],points[1]},0xd7e2e3)
  end
  block(-g.len-.6,-g.spool-.8,z-.25,3.8,.5,.5,0xd7e2e3)
  block(-g.len-.6,g.spool+.1,z-.25,3.8,.5,.5,0xd7e2e3)
  block(.7,-g.wide/2-2.8,z-.4,.7,3.1,.8,g.trim)
  block(.7,-g.wide/2-2.8,z-.4,3,.6,.8,g.trim)
  cylinder('y',3.7,-g.wide/2-3.1,z,1.25,.9,ink)
 else
  local r=g.radius;local z=-r-2;local cx=3.8
  block(0,-.7,-2.7,3,1.4,2.7,g.body)
  -- Open annulus and radial spokes: the sea really shows through the reel.
  local near,far=-g.wide/2,g.wide/2
  local function q(a,rr,y) return {cx+cos(a)*rr,y,z+sin(a)*rr} end
  for i=0,15 do
   local a,b=i*pi/8,(i+1)*pi/8
   face({q(a,r,near),q(b,r,near),q(b,r*.80,near),q(a,r*.80,near)},g.trim)
   face({q(a,r,near),q(a,r,far),q(b,r,far),q(b,r,near)},g.body)
   face({q(a,r*.80,near),q(b,r*.80,near),q(b,r*.80,far),q(a,r*.80,far)},g.body)
   face({q(a,r*.80,far),q(b,r*.80,far),q(b,r,far),q(a,r,far)},g.body)
  end
  cylinder('y',cx,near-.2,z,r*.32,g.wide+.2,g.body,g.trim)
  for i=0,g.spokes-1 do
   local a=i*2*pi/g.spokes
   face({q(a-.12,r*.28,near),q(a-.07,r*.83,near),q(a+.07,r*.83,near),q(a+.12,r*.28,near)},g.body)
  end
  cylinder('y',cx+r*.65,-g.wide/2-1.2,z,1,1,ink)
 end
 local width=1;local scratch={}
 while width<#faces do
  for first=1,#faces,width*2 do
   local middle=math.min(first+width,#faces+1)
   local last=math.min(first+width*2-1,#faces)
   local a,b=first,middle
   for k=first,last do
    if a<middle and (b>last or faces[a].z<=faces[b].z) then scratch[k]=faces[a];a=a+1
    else scratch[k]=faces[b];b=b+1 end
   end
  end
  for i=1,#faces do faces[i]=scratch[i] end
  width=width*2
 end
 return faces
end
local function reel_icon(item,x,y,scale,angle)
 local id
 for i,r in ipairs(reels) do if r==item then id=i;break end end
 if not id then return end
 if D.compile_mesh and not capture_commands then
  if not reel_mesh_handles[id] then
   reel_mesh_handles[id]=D.compile_mesh(build_reel_mesh(item,reel_profiles[id]))
  end
  D.draw_mesh(reel_mesh_handles[id],x,y,scale,angle,draw_x,clip_top,clip_bottom)
  frame_draw_calls=frame_draw_calls+1;return
 end
 if not reel_meshes[id] then reel_meshes[id]=build_reel_mesh(item,reel_profiles[id]) end
 local ca,sa=cos(angle),sin(angle)
 for _,f in ipairs(reel_meshes[id]) do
  local points={}
  for _,p in ipairs(f.p) do
   local grid=scale>3 and 2 or 1
   points[#points+1]={floor((x+(p[1]*ca-p[2]*sa)*scale)/grid+.5)*grid,floor((y+(p[1]*sa+p[2]*ca)*scale)/grid+.5)*grid}
  end
  poly(points,f.c)
 end
end
-- Authored pixel geometry: layered bodies, hardware and hanging treble hooks.
local function lure_icon(index,x,y,scale,disabled,angle,item)
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
 -- Clear diving bills: longer, shallower blades for deeper-running variants.
 -- Depth is a retrieve rating; it does not override floating/suspending buoyancy.
 local function bill(head)
  local profile=item and item.bill or 'short'
  local reach,drop,width=4,5,2
  if profile=='medium' then reach,drop,width=8,6,2.8
  elseif profile=='long' then reach,drop,width=13,6,3.5 end
  if index==4 then reach=reach+1;width=width+.7 end
  local tip=head-reach
  p({{head-1,1},{tip-1,drop-1},{tip,drop+1},{tip+width,drop+2},{head+3,3}},0x456475)
  p({{head,1.5},{tip,drop-1},{tip+.5,drop},{tip+width,drop+1},{head+2,2.5}},0x9fbfc7)
  l(head,2,tip+.5,drop-.5,0xe1efeb)
  l(tip+1,drop+1,tip+width,drop+1,0xc2d9d8)
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
   bill(-14)
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
   bill(-11)
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
 local yy=y
 while yy<y+h do
  local next_y=math.min(y+h,(floor(yy/31)+1)*31)
  rect(x,yy,w,next_y-yy,mix(wood(yy),c,alpha));yy=next_y
 end
end
-- Side-on underwater fish, entirely polygon/line geometry. Coordinates are
-- local to each fish; shared construction retains distinct species silhouettes.
local function fish_icon(f,x,y,scale,phase,angle,tint,flex,projection)
 local body,back,belly,fin=rgb(f.body),rgb(f.back),rgb(f.belly),rgb(f.fin)
 local shade=mix(back,C.ink,.45)
 local ca,sa=cos(angle or 0),sin(angle or 0)
 local function point(a,b)
  b=b+(flex or 0)*(a/30)^2
  local px,py=x+(a*ca-b*sa)*scale,y+(a*sa+b*ca)*scale
  if projection then return projection(px,py) end
  return px,py
 end
 local function p(q,c) local v={};for _,a in ipairs(q) do local px,py=point(a[1],a[2]);v[#v+1]={px,py} end;poly(v,tint or c) end
 local function l(a,b,c,d,k) local ax,ay=point(a,b);local bx,by=point(c,d);line(ax,ay,bx,by,tint or k) end
 local function dot(a,b,size,c)
  if projection then
   local q={};for _,v in ipairs({{a,b},{a+size,b},{a+size,b+size},{a,b+size}}) do
    local px,py=point(v[1],v[2]);q[#q+1]={px,py}
   end
   poly(q,tint or c)
  else local px,py=point(a,b);rect(px,py,math.max(1,size*scale),math.max(1,size*scale),tint or c) end
 end
 local shape=f.shape
 local tall=shape=='bream' or shape=='oval' or shape=='rabbit' or shape=='trevally' or shape=='scat' or shape=='giant'
 local h=tall and 12 or (shape=='puffer' and 10 or ((shape=='long' or shape=='slender' or shape=='ribbon') and 4 or (shape=='tuna' and 9 or 7)))
 local nose=shape=='long' and -29 or -23
 local tail=shape=='puffer' and 14 or 22
 local fork=shape~='rock' and shape~='grouper' and shape~='bass' and shape~='puffer' and shape~='giant'
 local sway=sin(phase or 0)*1.4
 if shape=='ribbon' then
  l(17,0,27,3,body);l(27,3,34,0,body);l(34,0,39,-3,belly)
 elseif fork then p({{tail-2,0},{tail+12,-h-2+sway},{tail+7,-1+sway},{tail+12,h+2+sway},{tail-2,2}},fin)
 else p({{tail-3,-2},{tail+8,-h*.75+sway},{tail+10,-h*.35+sway},{tail+10,h*.55+sway},{tail+6,h*.8+sway},{tail-3,3}},fin) end
 if shape~='puffer' then
  local top={ {-16,-h*.55} }
  if shape=='mahi' then top={{-21,-5},{-20,-14},{-12,-15},{1,-12},{17,-6},{22,-2}}
  elseif shape=='mullet' or shape=='slender' or shape=='long' then
   top={{-9,-h},{-8,-h-5},{-3,-h-2},{0,-h},{8,-h},{11,-h-4},{17,-2}}
  elseif shape=='tuna' then
   top={{-12,-h*.8},{-7,-h-6},{-3,-h},{4,-h*.7},{10,-h-10},{9,-h*.2},{18,-2}}
  else
   for i=0,6 do top[#top+1]={-13+i*4,-h-(i%2==0 and (shape=='rock' and 4 or 1.7) or 0)} end
   top[#top+1]={18,-2}
  end
  p(top,fin)
  p({{0,h*.65},{8,h+5},{13,h+2},{17,2}},fin)
 end
 local q
 if shape=='rock' then q={{-24,0},{-23,-6},{-17,-11},{-9,-9},{2,-7},{13,-4},{22,-2},{22,3},{11,5},{0,9},{-13,10},{-23,5}}
 elseif shape=='giant' then q={{-26,1},{-25,-7},{-17,-13},{-5,-12},{9,-7},{22,-2},{22,3},{8,8},{-6,12},{-20,9},{-26,5}}
 elseif shape=='scat' then q={{-21,0},{-16,-7},{-7,-15},{4,-14},{13,-5},{20,-1},{20,2},{12,8},{0,14},{-12,10},{-19,4}}
 elseif shape=='ribbon' then q={{-29,0},{-25,-5},{-17,-5},{0,-3},{19,-1},{21,1},{3,3},{-18,5},{-28,4}}
 elseif shape=='mullet' then q={{-24,-1},{-23,-5},{-10,-7},{4,-6},{22,-2},{22,2},{5,6},{-12,7},{-23,3}}
 elseif shape=='long' then q={{-30,1},{-27,-2},{-19,-4},{-7,-5},{10,-3},{23,-1},{23,2},{6,4},{-15,4},{-26,5},{-30,3}}
 elseif shape=='mahi' then q={{-24,4},{-24,-3},{-21,-11},{-16,-12},{-7,-10},{8,-5},{22,-1},{22,2},{8,5},{-9,7},{-20,7}}
 elseif shape=='puffer' then q={{-23,0},{-20,-6},{-12,-10},{-3,-11},{7,-7},{14,-2},{16,1},{12,5},{3,10},{-11,10},{-20,6}}
 else q={{nose,0},{-19,-h*.55},{-11,-h},{-2,-h},{9,-h*.65},{tail,-2},{tail,2},{9,h*.65},{-5,h},{-17,h*.65},{nose,3}} end
 p(q,shade)
 local inner={};for _,a in ipairs(q) do inner[#inner+1]={a[1]*.97,a[2]*.88} end;p(inner,body)
 p({{nose+3,2},{-8,3},{8,2},{tail-2,1},{8,h*.55},{-5,h*.8},{-17,h*.48}},belly)
 p({{nose+4,-1},{-11,-h*.85},{-2,-h*.86},{9,-h*.52},{tail-1,-1},{-3,-h*.4}},back)
 if f.mark=='redbands' then
  p({{-19,-6},{-15,-8},{-12,7},{-16,6}},rgb(0xa35151))
  p({{-5,-10},{0,-10},{7,8},{2,10}},rgb(0xb25b56))
  p({{13,-5},{17,-3},{21,1},{17,4}},rgb(0xb25b56))
 elseif f.mark=='goldstripe' then
  l(-21,0,22,0,rgb(0xc9b85d));l(-20,-5,-13,4,back)
 elseif f.mark=='bellylines' then
  for i=0,2 do l(-11,3+i*1.8,12,2+i,back) end
 elseif f.mark=='bars' then
  for i=0,5 do local a=-9+i*4;l(a,-h*.55,a-1,h*.3,mix(back,body,.3)) end
 elseif f.mark=='lines' then
  for i=0,2 do l(-13,-3+i*3,10,-2+i*2,rgb(0xb2a568)) end
 elseif f.mark~='plain' then
  for row=0,2 do for i=0,6 do
   local a=-13+i*4+(row%2)*2;local b=-h*.5+row*h*.4
   if a<14 then
    local c=f.mark=='orange' and rgb(0xad6545) or (f.mark=='blue') and rgb(0x66bfcc) or ((f.mark=='dots' and shape=='puffer') and belly or back)
    dot(a,b,(f.mark=='mottle' or f.mark=='cloud' or f.mark=='bigdots') and 2.5 or .85,c)
   end
  end end
 end
 p({{-9,1},{-2,2},{3,7},{-5,5}},fin)
 l(-12,-h*.45,-10,0,shade);l(-10,0,-12,h*.55,shade)
 local eye=shape=='long' and -23 or -18
 dot(eye-1,-3.5,3,belly);dot(eye,-3,1.8,C.ink);dot(eye,-3,.7,C.white)
 l(nose,1,nose+6,2,shade)
 if shape=='long' then l(-28,3,-20,3,back);dot(-25,2,1,C.cream) end
 if shape=='tuna' or shape=='torpedo' then for i=0,3 do p({{15+i*2,-2},{16+i*2,-4},{17+i*2,-2}},fin) end end
end
local function draw_menu(which,offset)
 local tab=which or tab
 draw_x=offset or 0
 for y=0,447,31 do rect(0,y,368,math.min(31,448-y),wood(y)) end
 for y=0,447,31 do
  rect(0,y+27,368,3,rgb(0x6a4325))
  for n=0,4 do local x=(n*83+floor(y/31)*47)%368
   rect(x,y+5+(n%3)*5,28+n*3,3,rgb(0x7b512e))
   rect(x+12,y+18,17,2,rgb(0x734b2b))
  end
 end
 border(0,0,368,448,rgb(0x50371f),2)
 local dark=rgb(0x2a2319)
 for i,name in ipairs({'RODS','REELS','LURES','BAG'}) do
  local x=16+(i-1)*86
  panel(x,16,80,36,i==tab and rgb(0xe6d1a2) or dark,i==tab and .81 or .80)
  centered(x,27,80,name,i==tab and C.darkwood or C.cream,2)
 end
 local list=catalog(tab)
 clip_top,clip_bottom=62,350
 local first_row=floor(scrolls[tab]/96)
 for slot=1,12 do
  local i=first_row*3+slot
  local x=32+(i-1)%3*112;local y=66+floor((i-1)/3)*96-scrolls[tab]
  if i<=math.max(9,math.ceil(#list/3)*3) then
  panel(x,y,88,88,dark,.70)
  rect(x,y,88,1,rgb(0x664627));rect(x,y+87,88,1,rgb(0x5c3d23))
  local item=list[i]
  if item then
   local enabled=tab==4 or tab==1 or compatible(rod,item,tab==2)
   if tab==1 then
    rod_thumbnail(item,x,y)
    local length=rod_length(item)
    rect(x+4,y+3,13,15,C.ink)
    text(x+6,y+5,rod_mark(item),C.cream,1.5,true)
    text(x+84-(#length*6-1)*1.5,y+5,length,C.cream,1.5)
   elseif tab==2 then inventory_reel(item.kind,x+44,y+46,not enabled,i==2 and C.red or (i==3 and C.gold or (i==4 and C.cyan or C.gray)))
   elseif tab==4 then
    panel(x+2,y+2,84,84,rgb(0x174052),.72)
    fish_icon(item,x+38,y+39,1.5,0,pi/5)
   else lure_icon(item.icon,x+47,y+41,(item.icon==1 or item.icon==4) and 1.65 or 2.1,not enabled,0,item) end
   if i==focus[tab] then border(x-2,y-2,92,92,C.cream,3) end
   local equipped=tab==1 and rod_index or (tab==2 and reel_index or lure_index)
   if tab~=4 and i==equipped then local ey=tab==1 and y+76 or y+5;rect(x+5,ey,7,7,C.green);rect(x+6,ey+1,3,3,C.white) end
  end
 end
 end
 clip_top,clip_bottom=0,448
 -- Tick rail: neighbouring marks broaden around the scroll position.
 -- Fits the existing right gutter without overlaying equipment cells.
 local scroll_limit=max_scroll(tab)
 if scroll_limit>0 then
  local position=35*clamp(scrolls[tab]/scroll_limit,0,1)
  local current=floor(position+.5)
  for i=0,35 do
   local distance=math.abs(i-position)
   local width=3+floor(12*math.exp(-distance*distance/3)+.5)
   local color=mix(rgb(0x886849),C.cream,.28+.23*math.exp(-distance*distance/5))
   if i==current then width=16;color=rgb(0x30261d) end
   rect(348,66+i*8,width,i==current and 3 or 2,color)
  end
 end
 panel(14,360,340,85,dark,.70)
 if tab==4 then
  local item=list[focus[4]]
  if item then
   local name_scale=math.min(2.5,316/(#item.name*6-1))
   text(26,373,item.name,C.cream,name_scale)
   rect(26,398,316,1,rgb(0x785b38))
   centered(24,413,96,item.cm..' CM',C.cyan,2.25)
   centered(128,413,106,item.kg..' KG',C.cream,2.25)
   rect(122,408,1,27,rgb(0x785b38));rect(238,408,1,27,rgb(0x785b38))
   -- Pixel coin identifies total catch value; length and weight keep units.
   rect(249,413,10,16,rgb(0x97702e));rect(246,416,16,10,C.gold)
   rect(249,415,9,11,rgb(0xe9bd58));rect(251,417,2,7,C.cream)
   local price=tostring(fish_value(item))
   local price_scale=math.min(2.5,74/(#price*6-1))
   text(342-(#price*6-1)*price_scale,413,price,C.gold,price_scale)
  else
   centered(0,190,368,'NO CATCH YET',C.cream,2)
   centered(14,391,340,'FISH BAG / 0 CATCHES',C.cream,1.5)
  end
  draw_x=0;return
 end
 local item=list[focus[tab]];if not item then draw_x=0;return end
 -- Four fields only; the wider right column accommodates series names.
 local function cell(column,y,value,color)
  local width=column==1 and 124 or 188
  local scale=2
  for _,candidate in ipairs({2,1.75,1.5,1.25,1}) do
   scale=candidate
   if (#value*6-1)*scale<=width then break end
  end
  if (#value*6-1)*scale>width then
   value=value:sub(1,floor((width+1)/6)-3)..'...'
  end
  text(column==1 and 25 or 157,y,value,color or C.cream,scale)
 end
 logo(item.brand,25,376)
 cell(2,378,item.name,C.white)
 if tab==1 then
  cell(1,416,item.label..item.power)
  cell(2,416,item.action)
 elseif tab==2 then
  local model=item.model:gsub(' /.*',''):gsub(' RIGHT',''):gsub(' LEFT','')
  cell(1,416,model)
  cell(2,416,item.capacity)
 else
  local model=item.model:gsub(' /.*','')
  local weight='N/A'
  if item.kind=='lure' and not item.weight_estimated then
   weight=string.format('%.1f',item.weight):gsub('%.0$','')..'G'
  end
  cell(1,416,model)
  cell(2,416,weight)
 end
 draw_x=0

end
-- Reference landmarks measured after fitting the approved board to 368x448.
local wave_positions={
 {179,224,22},{105,237,26},{329,246,22},{22,256,26},{216,264,23},
 {122,332,29},{29,376,29},{205,384,28},
}
-- Game-world weather, not a live forecast. One day lasts 30 real minutes.
local Weather={cache=nil,seed=A.check=='1' and 719 or math.floor(system.millis()%2147483646)+1}
Weather.presets={calm={name='SUNNY',cloud=0,cool=0,rain=0},sunny={name='SUNNY',cloud=0,cool=0,rain=0},cloudy={name='CLOUDY',cloud=.65,cool=1.5,rain=0},rain={name='RAIN',cloud=1,cool=3,rain=1}}
-- Game tuning, not biological measurements. Shared by HUD, waves and encounters.
function Weather.wind_level(wind)
 return wind<.5 and 0 or (wind<2.5 and 1 or (wind<4.5 and 2 or 3))
end
Weather.wave_sizes={.35,1,1.5,2.2}
Weather.feeding={
 day={wind={.8,1,1.15,.65},night=.35,day=1,dawn=1.15},
 twilight={wind={.75,1,1.3,.75},night=.7,day=.75,dawn=1.5},
 night={wind={.85,1,1.1,.6},night=1.35,day=.45,dawn=1.1},
}
Weather.fish_periods={'night','twilight','day','day','day','day','twilight','day','day','twilight',
 'day','day','twilight','twilight','day','day','twilight','day','day','twilight',
 'twilight','twilight','twilight','twilight','twilight','day','twilight','night','day','day'}
function Weather.feeding_factor(index,w)
 local profile=Weather.feeding[Weather.fish_periods[index] or 'twilight']
 local hour=w.hour
 local daylight=clamp(math.min((hour-5)/2,(20-hour)/2),0,1)
 local peak=math.max(0,1-math.abs(hour-6)/2,1-math.abs(hour-18)/2)
 local time=profile.night*(1-daylight)+profile.day*daylight
 time=time+(profile.dawn-time)*peak
 local wind=profile.wind[Weather.wind_level(w.wind)+1]
 return time*wind,time,wind
end
-- Seeded daily targets stay within +/-0.35 of the session's prevailing wind.
-- Smoothstep joins targets across midnight without jumps or frame-based RNG.
function Weather.wind_at(days,seed)
 seed=seed or Weather.seed
 local function random(day)
  local value=(seed+(day+1)*104729)%2147483647
  value=(value*48271)%2147483647
  value=(value*48271)%2147483647
  return value/2147483647
 end
 local base=random(-1)*5.5
 local day=math.floor(days);local u=days-day;u=u*u*(3-2*u)
 local a=clamp(base+(random(day)*2-1)*.35,0,5.5)
 local b=clamp(base+(random(day+1)*2-1)*.35,0,5.5)
 return a+(b-a)*u
end
function Weather.sample(t,forced_hour,forced_weather)
 local elapsed_minutes=(forced_hour or tonumber(A.hour) or 9)*60+t*.8
 local minutes=elapsed_minutes%1440
 local hour=minutes/60
 local mode=forced_weather or A.weather or 'auto'
 local transition=0;local prior
 if mode=='auto' then
  local schedule={'sunny','cloudy','rain','cloudy','sunny','calm'}
  local epoch=math.floor(t/180);mode=schedule[epoch%6+1]
  prior=Weather.presets[schedule[(epoch-1)%6+1]]
  transition=clamp((t%180)/25,0,1)
 end
 local state=Weather.presets[mode] or Weather.presets.sunny
 local cloud=prior and (prior.cloud*(1-transition)+state.cloud*transition) or state.cloud
 local wind=Weather.wind_at(elapsed_minutes/1440)
 local cooling=prior and (prior.cool*(1-transition)+state.cool*transition) or state.cool
 local rain_amount=prior and (prior.rain*(1-transition)+state.rain*transition) or state.rain
 local stops={
  {0,0x08182f,0x233348,0x123746,0x082735},
  {5,0x102744,0x685369,0x26495b,0x10384c},
  {6.5,0x398eb4,0xf2c18a,0x34879d,0x12657e},
  {9,0x29b6fd,0x7bdaff,0x0072d3,0x005ebc},
  {14,0x29b6fd,0x8edcfa,0x087fcc,0x075cae},
  {16.5,0x719cc7,0xffc488,0x3a8ba3,0x286987},
  {18,0x746987,0xfa9a63,0x756a84,0x344e6c},
  {19,0x24344f,0x986375,0x364e6b,0x182f4b},
  {21,0x08182f,0x233348,0x123746,0x082735},
  {24,0x08182f,0x233348,0x123746,0x082735},
 }
 local a,b=stops[1],stops[2]
 for i=1,#stops-1 do if hour>=stops[i][1] then a,b=stops[i],stops[i+1] end end
 local u=(hour-a[1])/(b[1]-a[1]);local colors={}
 local daylight=clamp(1-math.abs(hour-12)/7,0,1)
 for i=2,5 do
  local c=mix(rgb(a[i]),rgb(b[i]),u)
  colors[i-1]=mix(c,mix(rgb(0x162936),rgb(0x7b98a3),daylight),cloud*.65)
 end
 local night=hour<6 or hour>=20
 return {hour=hour,minutes=math.floor(minutes),name=state.name,cloud=cloud,wind=wind,
  temperature=math.floor(24+5*sin((hour-8)*pi/12)-cooling+.5),
  sky=colors[1],horizon=colors[2],sea=colors[3],deep=colors[4],
  foam=mix(colors[3],rgb(night and 0x708d9e or 0xd7f2eb),.65),night=night,
  sun_x=440-(hour-14)*35,sun_y=26+177*(math.max(0,hour-14)/4)^1.35,
  sun_visible=hour>=14 and hour<18.5 and cloud<.9,rain=rain_amount>0,rain_amount=rain_amount}
end
function Weather.draw(t)
 local bucket=math.floor(t)
 if not Weather.cache or Weather.bucket~=bucket then
  Weather.cache=Weather.sample(t);Weather.bucket=bucket
 end
 local w=Weather.cache
 C.sky,C.sea1,C.sea2,C.foam=w.sky,w.sea,w.deep,w.foam
 for i=0,7 do rect(0,i*26,368,math.min(26,203-i*26),mix(w.sky,w.horizon,i/7)) end
 if w.night then
  for i=1,13 do local x=(i*73)%368;local y=61+(i*31)%112;rect(x,y,1,1,mix(w.sky,C.white,.55)) end
 end
 if w.sun_visible then
  local old=clip_bottom;clip_bottom=203
  local color=mix(rgb(0xffedb0),rgb(0xff9b54),clamp((w.hour-16)/2,0,1))
  ellipse(w.sun_x,w.sun_y,19,19,color)
  clip_bottom=old
 end
 rect(0,203,368,95,w.sea);rect(0,298,368,150,w.deep)
 if w.sun_visible and w.sun_x<390 then
  for i=0,10 do
   local width=8+i*3+sin(t*1.4+i)*4
   rect(w.sun_x-width/2+sin(t+i)*3,208+i*10,width,2,mix(w.sea,rgb(0xffc884),.48-i*.025))
  end
 end
 local cloud_color=mix(w.horizon,w.night and rgb(0x3b4b61) or rgb(0xf5f6e9),.65-w.cloud*.3)
 local count=3+math.floor(w.cloud*4)
 for i=1,count do
  local x=((i*103-34+t*w.wind*.8)%438)-45;local y=67+(i*29)%88
  rect(x,y,44+i%3*7,10,cloud_color);rect(x+12,y-9,24,9,cloud_color)
 end
 local wave_size=Weather.wave_sizes[Weather.wind_level(w.wind)+1]
 for i,wave in ipairs(wave_positions) do
  local x=wave[1]+floor(sin(t*(.3+w.wind*.1)+i)*(1.5+w.wind*.65))
  local y=wave[2]+floor(sin(t*.5+i)*wave_size*1.5)
  local width=math.max(4,floor(wave[3]*wave_size))
  local height=math.max(1,floor(2*wave_size))
  rect(x-(width-wave[3])/2,y+height,width,height,w.foam)
  rect(x-(width-wave[3])/2+width*.2,y,width*.6,height,w.foam)
 end
 if w.wind>=4 then
  for i=1,10 do
   local x=(i*67+t*w.wind*2)%368;local y=248+i%5*32
   rect(x,y,8,1,mix(w.foam,w.sea,.3))
  end
 end
 if w.rain then
  -- Unequal speeds and hashed lanes avoid a moving grid. Far drops are short
  -- and faint; foreground drops are longer. Wind only gently tilts rainfall.
  for i=1,36 do
   local depth=((i*47)%101)/100
   local speed=150+depth*170
   local travel=t*speed+((i*137)%449)
   local cycle=math.floor(travel/480)
   local y=travel%480-16
   local x=((i*97+i*i*31+cycle*73)%430)-30-(y/448)*w.wind*3
   local length=3+depth*9
   local background=y<203 and w.horizon or (y<298 and w.sea or w.deep)
   local color=mix(background,rgb(0xc3d8de),(.14+depth*.3)*w.rain_amount)
   line(x,y,x-w.wind*length*.09,y+length,color)
  end
  -- Brief, expanding surface rings at independent impact points. Perspective
  -- makes near rings larger; no particles or texture allocations are needed.
  for i=1,14 do
   local clock=t*(.85+(i%4)*.13)+i*.371
   local cycle=math.floor(clock);local age=clock-cycle
   if age<.38 then
    local z=((i*79+cycle*43)%229)/229
    local y=210+z*222
    local x=8+(i*113+cycle*157)%350
    local radius=(1+age*9)*(.35+z*.85)
    local bg=y<298 and w.sea or w.deep
    local color=mix(bg,rgb(0xc4dfdf),(.4-age)*w.rain_amount)
    line(x-radius,y,x+radius,y,color)
    if z>.4 then
     line(x-radius*.55,y-1,x+radius*.55,y-1,color)
     line(x-radius*.55,y+1,x+radius*.55,y+1,color)
    end
   end
  end
 end
end
function Weather.hud()
 local w=Weather.cache;if not w then return end
 local key=w.minutes..':'..w.temperature..':'..Weather.wind_level(w.wind)..':'..tostring(w.night)
 local cacheable=D.compile_commands and not capture_commands
 if cacheable and Weather.hud_key==key then
  D.draw_commands(Weather.hud_commands);frame_draw_calls=frame_draw_calls+1;return
 end
 if cacheable then capture_commands={} end
 local fg=w.night and rgb(0xe4e9d7) or rgb(0xfaffef)
 local function label(x,y,value,scale)
  text(x+1,y+1,value,rgb(0x234455),scale);text(x,y,value,fg,scale)
 end
 local hour=math.floor(w.minutes/60)
 label(16,26,string.format('%d:%02d %s',(hour+11)%12+1,w.minutes%60,hour<12 and 'AM' or 'PM'),2)
 -- Four wind levels: calm, one, two or three wave strokes.
 local level=Weather.wind_level(w.wind)
 for row=0,level-1 do
  for i=0,11 do
   local x=253+i*2;local y=34+(row-(level-1)/2)*5+floor(sin(i*.65)*1.5)
   rect(x+1,y+1,3,2,rgb(0x234455));rect(x,y,3,2,fg)
  end
 end
 label(296,26,tostring(w.temperature)..' C',2)
 if cacheable then
  Weather.hud_commands=D.compile_commands(capture_commands);capture_commands=nil;Weather.hud_key=key
  D.draw_commands(Weather.hud_commands);frame_draw_calls=frame_draw_calls+1
 end
end
local boat_commands
local function sea(t)
 Weather.draw(t)
 if boat_commands then D.draw_commands(boat_commands);frame_draw_calls=frame_draw_calls+1;return end
 local cacheable=D.compile_commands and not capture_commands
 if cacheable then capture_commands={} end
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
 if cacheable then
  boat_commands=D.compile_commands(capture_commands);capture_commands=nil
  D.draw_commands(boat_commands);frame_draw_calls=frame_draw_calls+1
 end
end
-- Bending is a distributed curvature along the blank, not a scaled sprite.
-- Power controls compliance; Action moves the curvature onset toward the tip.
local function bend_at(s,power,action,length,load)
 local onset=action_start[action]
 local u=math.max(0,(s-onset)/(1-onset))
 return load/stiffness[power]*(length/6.25)^1.35*u*u
end
-- Reduced-order elastic beam. Public inputs use metres, kg, seconds and radians.
-- EI is game-calibrated: manufacturers do not publish blank stiffness curves.
local RodPhysics={}
function RodPhysics.new(length_m,power,action)
 assert(length_m>0 and stiffness[power] and action_start[action])
 local stiff=stiffness[power]
 return {length_m=length_m,power=power,action=action,q=0,v=0,angle=-1.98,
  last_angle=-1.98,last_speed=0,accumulator=0,
  omega=clamp(18*math.sqrt(stiff)/(length_m/1.9)^1.3,5,30),
  damping=.16+.05*stiff,EI=24*stiff}
end
function RodPhysics.step(state,dt,input)
 if Physics.rod_step then return Physics.rod_step(state,dt,input or {}) end
 input=input or {};dt=clamp(dt,0,.25)
 state.accumulator=state.accumulator+dt
 local h=1/120
 while state.accumulator>=h do
  state.accumulator=state.accumulator-h
  local target=input.handle_angle or -1.98
  local delta=clamp(target-state.angle,-8*h,8*h)
  state.angle=state.angle+delta
  local speed=delta/h
  local acceleration=clamp((speed-state.last_speed)/h,-120,120)
  state.last_speed=speed
  local fish=math.max(0,input.fish_kg or 0)
  -- Fish are buoyant; only a fraction of weight is transmitted as line tension.
  local tension=math.min(fish*9.81*(input.fish_pull or .25),input.drag_n or 80)
  local lure=math.max(0,input.lure_g or 0)/1000
  local force=tension+math.max(0,input.line_n or 0)+lure*(9.81+math.abs(acceleration)*state.length_m*.3)
  local raw=-force*state.length_m^2/(2*state.EI)
  local equilibrium=1.5*raw/(1+math.abs(raw))
  local aa=state.omega^2*(equilibrium-state.q)-2*state.damping*state.omega*state.v-acceleration*.42
  state.v=clamp(state.v+aa*h,-18,18)
  state.q=clamp(state.q+state.v*h,-1.6,1.6)
 end
 return state
end
function RodPhysics.points(state)
 -- Preserve the approved storyboard's screen-space blank, then rotate each
 -- tangent by the simulated handle motion and distributed elastic deflection.
 local pts={{367,447}}
 local scale=clamp(1+(state.length_m-2.03)*.10,.90,1.15)
 local onset=action_start[state.action]
 local last_x,last_y=367,447
 for i=1,64 do
  local u=i/64;local v=1-u
  local x=v^3*367+3*v*v*u*339+3*v*u*u*278+u^3*236
  local y=v^3*447+3*v*v*u*345+3*v*u*u*195+u^3*123
  local shape=math.max(0,(u-onset)/(1-onset))^1.5
  local rotation=state.angle+1.98+state.q*shape
  local dx,dy=(x-last_x)*scale,(y-last_y)*scale
  local last=pts[#pts]
  pts[#pts+1]={last[1]+dx*cos(rotation)-dy*sin(rotation),last[2]+dx*sin(rotation)+dy*cos(rotation)}
  last_x,last_y=x,y
 end
 return pts
end
local function physics_for(r)
 return RodPhysics.new(r.feet*.3048,r.sim_power or r.power,r.sim_action or r.action)
end
local fish_mass=tonumber(A.fish_kg) or 2
local sim={state='ready',rod=nil,physics=nil,elapsed=0,fish_kg=fish_mass}
local function reset_sim()
 sim={state='ready',rod=rod,reel_index=reel_index,lure_index=lure_index,physics=physics_for(rod),elapsed=0,fish_kg=fish_mass,accumulator=0}
end
local function equipped_lure() return lures[lure_index] end
local function rod_points(r,mode,t,load)
 local state=physics_for(r)
 local duration=mode=='fight' and 3 or math.max(0,t)
 for i=1,math.min(720,floor(duration*120)) do
  local time=i/120
  local angle=-1.98
  if mode~='idle' and mode~='fight' then
   if time<.45 then angle=-1.98+time*.35
   elseif time<.65 then angle=-1.8225-(time-.45)*1.3 end
  end
  RodPhysics.step(state,1/120,{handle_angle=angle,lure_g=mode=='idle' and 0 or (load or 14),fish_kg=mode=='fight' and fish_mass or 0})
 end
 return RodPhysics.points(state)
end
local function draw_rod(r,reel,mode,t,ghost,physical_points)
 local lure=equipped_lure()
 local pts=physical_points or rod_points(r,mode,t,lure and lure.weight or 0)
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
 if D.stroke_path and not capture_commands then
  if not r.stroke_style then
   local outer,inner,colors={},{},{}
   for i=2,65 do
    local grip=i<(r.kind=='fly' and 16 or 14)
    outer[i-1]=grip and 10 or (i<45 and 7 or 4)
    inner[i-1]=grip and 7 or (i<45 and 4 or 2)
    colors[i-1]=grip and (r.kind=='fly' and i>5 and C.cork or C.grip) or C.gold
   end
   r.stroke_style={outer,inner,colors}
  end
  D.stroke_path(pts,r.stroke_style[1],C.ink,draw_x,clip_top,clip_bottom)
  D.stroke_path(pts,r.stroke_style[2],r.stroke_style[3],draw_x,clip_top,clip_bottom)
  frame_draw_calls=frame_draw_calls+2
 else
 for i=2,#pts do
  local a,b=pts[i-1],pts[i]
  stroke(a[1],a[2],b[1],b[2],i<(r.kind=='fly' and 16 or 14) and 10 or (i<45 and 7 or 4),C.ink)
 end
 for i=2,#pts do
  local a,b=pts[i-1],pts[i]
  local grip=i<(r.kind=='fly' and 16 or 14)
  stroke(a[1],a[2],b[1],b[2],grip and 7 or (i<45 and 4 or 2),grip and (r.kind=='fly' and i>5 and C.cork or C.grip) or C.gold)
 end
 end
 if physical_points then
  local a,b=pts[1],pts[2];local dx,dy=a[1]-b[1],a[2]-b[2];local len=math.sqrt(dx*dx+dy*dy)
  if len>.001 then stroke(a[1],a[2],a[1]+dx/len*65,a[2]+dy/len*65,10,C.ink)
   stroke(a[1],a[2],a[1]+dx/len*65,a[2]+dy/len*65,7,C.grip) end
 end
 local band=pts[r.kind=='fly' and 16 or 14]
 stroke(band[1]-2,band[2]+1,band[1]+3,band[2]-1,3,C.shine)
 local mount=pts[r.kind=='fly' and 3 or 15]
 local nextp=pts[r.kind=='fly' and 4 or 16]
 -- atan2 handles all quadrants; rod points up/left.
 local angle=math.atan(nextp[2]-mount[2],nextp[1]-mount[1])
 -- Mesh X points toward the butt, opposite the blank tangent.
 local side_angle=angle+pi
 if reel then
  local selected=reels[reel_index]
  if selected then reel_icon(selected,mount[1],mount[2],3.0,side_angle) end
 end
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
local function scene_draw(mode,t,r,reel,physical_points)
 sea(t)
 local tip=draw_rod(r,reel,mode,t,not physical_points,physical_points)
 if not equipped_lure() then return end
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
   local bait=equipped_lure()
   if bait.surface then lure_icon(bait.icon,x,y,.95,false,0,bait) else submerged_lure(bait.icon,x,y+3) end
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
  lure_icon(mode=='iso' and 7 or (lures[lure_index] or lures[1]).icon,x-3,y+14,1.15,false,mode=='overhead' and -.85 or -.2,lures[lure_index])
 end
end
-- Metres / seconds. Fixed 240 Hz line integration with compliant distance
-- constraints (XPBD); no screen-space flight interpolation or drawn fake arc.
local CastWorld={h=Budget.h,focal=260,eye=1.6,horizon=203,near=.25}
local function copy3(p) return {p[1],p[2],p[3]} end
local function length3(x,y,z) return math.sqrt(x*x+y*y+z*z) end
local function project3(p)
 local z=math.max(CastWorld.near,p[3])
 return {184+CastWorld.focal*p[1]/z,CastWorld.horizon+CastWorld.focal*(CastWorld.eye-p[2])/z},CastWorld.focal/z
end
local function world_rod(state,tip_only)
 local base,points,screen=state.world_base or {}, {}, {}
 if not state.world_base then
 -- Rest pose is the approved storyboard. The dimensional rod then rotates in
 -- its casting plane, including movement away from / toward the camera.
 for i=0,64 do local u=i/64;local v=1-u
  local x=v^3*367+3*v*v*u*339+3*v*u*u*278+u^3*236
  local y=v^3*447+3*v*v*u*345+3*v*u*u*195+u^3*123
  local z=1.3+.5*u
  base[i+1]={(x-184)*z/260,1.6+(203-y)*z/260,z}
 end
  state.world_base=base
 end
 if Physics.rod_pose then return Physics.rod_pose(state,tip_only,action_start[state.action]) end
 points[1]=copy3(base[1]);screen[1]=project3(points[1])
 local axisx,axisy,axisz=.75,.5,.4330127
 -- Pivot lies 25 cm behind the grip, on its extended centre line. Rotation
 -- sweeps a circle in an oblique plane; perspective turns that into an ellipse.
 local a,b=base[1],base[2];local d=length3(b[1]-a[1],b[2]-a[2],b[3]-a[3])
 local rootvec={(b[1]-a[1])*.25/d,(b[2]-a[2])*.25/d,(b[3]-a[3])*.25/d}
 local pivot={a[1]-rootvec[1],a[2]-rootvec[2],a[3]-rootvec[3]}
 local angle=-(state.angle+1.98);local ca,sa=cos(angle),sin(angle)
 local x,y,z=rootvec[1],rootvec[2],rootvec[3];local dot=axisx*x+axisy*y+axisz*z
 points[1]={pivot[1]+x*ca+(axisy*z-axisz*y)*sa+axisx*dot*(1-ca),
  pivot[2]+y*ca+(axisz*x-axisx*z)*sa+axisy*dot*(1-ca),
  pivot[3]+z*ca+(axisx*y-axisy*x)*sa+axisz*dot*(1-ca)}
 screen[1]=project3(points[1])
 local scale=state.length_m/2.03
 local lastx,lasty,lastz=points[1][1],points[1][2],points[1][3]
 if not state.world_segments then
  state.world_segments={}
  local onset=action_start[state.action]
  for i=2,65 do
   local u=(i-1)/64;local a,b=base[i-1],base[i]
   local x,y,z=(b[1]-a[1])*scale,(b[2]-a[2])*scale,(b[3]-a[3])*scale
   local dot=axisx*x+axisy*y+axisz*z
   state.world_segments[i]={x,y,z,math.max(0,(u-onset)/(1-onset))^1.5,dot}
  end
 end
 local cy,sy=state.yaw and cos(state.yaw) or 1,state.yaw and sin(state.yaw) or 0
 for i=2,65 do
  local segment=state.world_segments[i];local shape=segment[4]
  local angle=-(state.angle+1.98)-(state.fight_dir and 0 or state.q)*shape
  local ca,sa=state.fight_dir and ca or cos(angle),state.fight_dir and sa or sin(angle)
  local x,y,z,dot=segment[1],segment[2],segment[3],segment[5]
  local dx=x*ca+(axisy*z-axisz*y)*sa+axisx*dot*(1-ca)
  local dy=y*ca+(axisz*x-axisx*z)*sa+axisy*dot*(1-ca)
  local dz=z*ca+(axisx*y-axisy*x)*sa+axisz*dot*(1-ca)
  if state.yaw then
   dx,dz=dx*cy+dz*sy,-dx*sy+dz*cy
  end
  if state.fight_dir then
   local len=length3(dx,dy,dz);local f=state.fight_dir
   local axial=(f[1]*dx+f[2]*dy+f[3]*dz)/math.max(1e-8,len)
   local gain=math.abs(state.q)*shape*1.5
   local nx=dx+len*gain*(f[1]-axial*dx/len)
   local ny=dy+len*gain*(f[2]-axial*dy/len)
   local nz=dz+len*gain*(f[3]-axial*dz/len)
   local norm=length3(nx,ny,nz)
   dx,dy,dz=nx*len/norm,ny*len/norm,nz*len/norm
  end
  lastx,lasty,lastz=lastx+dx,lasty+dy,lastz+dz
  if not tip_only then points[i]={lastx,lasty,lastz};screen[i]=project3(points[i]) end
 end
 if state.rail_blend and state.rail_blend>0 then
  -- Bring the rod over the visible rail while keeping the grip fixed and
  -- preserving every segment length. Fish remain in the water below the tip.
  local root=points[1]
  local ax,ay,az=lastx-root[1],lasty-root[2],lastz-root[3]
  local span=length3(ax,ay,az);ax,ay,az=ax/span,ay/span,az/span
  local tx,tz=1.05-root[1],1.85-root[3]
  local ty=math.sqrt(math.max(.01,span*span-tx*tx-tz*tz))
  local size=length3(tx,ty,tz);local blend=state.rail_blend
  local bx,by,bz=ax*(1-blend)+tx/size*blend,ay*(1-blend)+ty/size*blend,az*(1-blend)+tz/size*blend
  size=length3(bx,by,bz);bx,by,bz=bx/size,by/size,bz/size
  local kx,ky,kz=ay*bz-az*by,az*bx-ax*bz,ax*by-ay*bx
  local c=math.max(.001,1+ax*bx+ay*by+az*bz)
  lastx,lasty,lastz=root[1]+bx*span,root[2]+by*span,root[3]+bz*span
  if not tip_only then
   for i=2,65 do
    local p=points[i];local x,y,z=p[1]-root[1],p[2]-root[2],p[3]-root[3]
    local vx,vy,vz=ky*z-kz*y,kz*x-kx*z,kx*y-ky*x
    p[1]=root[1]+x+vx+(ky*vz-kz*vy)/c
    p[2]=root[2]+y+vy+(kz*vx-kx*vz)/c
    p[3]=root[3]+z+vz+(kx*vy-ky*vx)/c
    screen[i]=project3(p)
   end
  end
 end
 if tip_only then return {lastx,lasty,lastz} end
 return points,screen
end
local function water_profile(lure)
 local surface=lure.surface or lure.kind=='iso'
 local kind=surface and 'surface' or 'sink'
 if not surface then
  if lure.spec:find('SUSPEND') or lure.model:match('%dSP') then kind='suspend'
  elseif lure.spec:find('FLOAT') or lure.model:match('%dF') then kind='float' end
 end
 local mass=math.max(.001,lure.weight/1000)
 local mm=tonumber(lure.length:match('([%d%.]+)%s*MM'))
 local inches=tonumber(lure.length:match('([%d%.]+)%s*IN'))
 local size=mm and mm/1000 or (inches and inches*.0254 or .06)
 -- Displaced volumes / Cd are calibrated, not published model measurements.
 -- Weight alone cannot decide whether a hollow-bodied lure sinks.
 local displaced=kind=='suspend' and 1 or ((kind=='float' or surface) and 1.06 or (lure.spec:find('SLOW SINK') and .93 or .82))
 local area=size*(lure.icon==3 and .009 or .016)
 return {kind=kind,mass=mass,volume=mass*displaced/1025,
  acceleration=9.81*(displaced-1),drag=.5*1025*area*(lure.drag or 1),surface=surface}
end
local function wet_velocity(profile,x,y,z,h)
 local speed=length3(x,y,z)
 local factor=1/(1+profile.drag/profile.mass*speed*h)
 return x*factor,(y+profile.acceleration*h)*factor,z*factor
end
local function new_line(tip,lure,fly)
 local line={p={},prev={},rest={},lambda={},paid=fly and 5.4 or .65,
  density=fly and .0014 or .00013,axial=fly and 1200 or 8000,fly=fly,mass=math.max(.001,lure.weight/1000),
  tension=0,max_stretch=0,payout=0,landed=false,water=water_profile(lure)}
 local n=fly and 24 or 6
 for i=0,n do local u=i/n
  -- Fly line begins laid out in front, leader at the end. Casting strokes
  -- lift and accelerate its mass; the fly is not given a projectile impulse.
  local p=fly and {tip[1]-.35*u,tip[2]*(1-u),tip[3]+line.paid*.92*u}
   or {tip[1],tip[2]-line.paid*u,tip[3]}
  line.p[i+1]=p;line.prev[i+1]=copy3(p)
  if i>0 then line.rest[i]=line.paid/n end
 end
 return line
end
local function constraint(line,i,h)
 local a,b=line.p[i],line.p[i+1]
 local x,y,z=b[1]-a[1],b[2]-a[2],b[3]-a[3]
 local d=length3(x,y,z);if d<1e-8 then return end
 local rest=line.rest[i]
 local node_mass=math.max(.00002,line.density*rest)
 local wa=i==1 and 0 or 1/node_mass
 local wb=i+1==#line.p and 1/line.mass or 1/node_mass
 local alpha=(rest/line.axial)/(h*h)
 local old=line.lambda[i] or 0
 -- A fishing line carries tension only: compressed spans must not push
 -- adjacent particles apart like a chain of elastic rods.
 local next_lambda=math.min(0,old+(-(d-rest)-alpha*old)/(wa+wb+alpha))
 local dl=next_lambda-old
 line.lambda[i]=next_lambda
 local k=dl/d
 if wa>0 then a[1]=a[1]-wa*k*x;a[2]=a[2]-wa*k*y;a[3]=a[3]-wa*k*z end
 if wb>0 then b[1]=b[1]+wb*k*x;b[2]=b[2]+wb*k*y;b[3]=b[3]+wb*k*z end
end
local function step_line(line,tip,h,released,lure)
 local n=#line.p
 local last=line.p[n];local oldlast=line.prev[n]
 local vx,vy,vz=(last[1]-oldlast[1])/h,(last[2]-oldlast[2])/h,(last[3]-oldlast[3])/h
 local dx,dy,dz=last[1]-tip[1],last[2]-tip[2],last[3]-tip[3]
 local distance=length3(dx,dy,dz)
 -- Pay line out at the guide, inserting short spans there. Existing spans
 -- never inflate to manufacture length. A small surplus allows natural sag.
 if released and not line.landed then
  local outward=math.max(0,(dx*vx+dy*vy+dz*vz)/math.max(.01,distance))
  local deficit=math.max(0,distance+outward*h+(line.fly and .08 or .025)-line.paid)
  local rate=deficit/h
  local paid=math.min(rate*h,math.max(0,55-line.paid))
  line.paid=line.paid+paid;line.payout=rate;line.rest[1]=line.rest[1]+paid
  if line.rest[1]>.48 and n<Budget.nodes then
   local a,b=line.p[1],line.p[2];local u=.24/line.rest[1]
   local p={a[1]+(b[1]-a[1])*u,a[2]+(b[2]-a[2])*u,a[3]+(b[3]-a[3])*u}
   table.insert(line.p,2,p);table.insert(line.prev,2,copy3(p))
   local total=line.rest[1];line.rest[1]=.24;table.insert(line.rest,2,total-.24)
   n=#line.p
  end
 end
 if Physics.advance_rope then
  Physics.advance_rope(line,tip,h,released,lure.drag or 1,line.fish and 2 or Budget.iterations,
   sim.line_setup and sim.line_setup.drag or 0,sim.line_setup and sim.line_setup.capacity or 55)
 else
 if Physics.integrate_rope then Physics.integrate_rope(line,h,released,dx,dy,dz,distance,lure.drag or 1) else
 for i=2,n do
  local p,prev=line.p[i],line.prev[i]
  local x,y,z=p[1],p[2],p[3]
  local ux,uy,uz=(x-prev[1])/h,(y-prev[2])/h,(z-prev[3])/h
  local speed=length3(ux,uy,uz)
  if i==n and line.landed and y<=0 then
   ux,uy,uz=wet_velocity(line.water,ux,uy,uz,h)
   p[1]=x+ux*h;p[2]=(line.fish and line.fish.action=='jump') and y+uy*h or math.min(0,y+uy*h);p[3]=z+uz*h
   if line.surface then p[2]=0 end
  elseif i<n and line.landed and y<=0 then
   -- Buoyant braid/fly line can be pulled below water by the sinking rig.
   local factor=1/(1+5*h+.3*speed*h)
   p[1]=x+ux*factor*h;p[2]=math.min(0,y+(uy*factor+.15*h)*h);p[3]=z+uz*factor*h
  else
   -- Quadratic aerodynamic drag; heavier lures retain speed better.
   local drag=i==n and (.000045*(lure.drag or 1)/line.mass) or .016
   local factor=1/(1+drag*speed*h)
   ux,uy,uz=ux*factor,uy*factor,uz*factor
   if i<n and y<=.005 then ux=ux/(1+10*h);uz=uz/(1+10*h) end
   if released and i==n and not line.fly and distance>.1 then
    local resistance=(line.reel_kind=='spin' and .0015 or .0028)+speed*speed*.000012
    local decel=math.min(speed/h,resistance/line.mass)
    ux=ux-decel*dx/distance*h;uy=uy-decel*dy/distance*h;uz=uz-decel*dz/distance*h
   end
   p[1]=x+ux*h;p[2]=y+uy*h-9.81*h*h;p[3]=z+uz*h
   if i<n and p[2]<0 and not line.landed then p[2]=0 end
  end
  prev[1],prev[2],prev[3]=x,y,z
 end
 end
 if line.fish then
  local p=line.p[#line.p]
  local distance=length3(p[1]-tip[1],p[2]-tip[2],p[3]-tip[3])
  local drag=sim.line_setup.drag
  local required=distance/(1+drag/line.axial)-line.paid
  local paid=clamp(required,0,math.min(5*h,math.max(0,sim.line_setup.capacity-line.paid)))
  line.payout=paid/h
  if paid>0 then
   line.paid=line.paid+paid;line.rest[1]=line.rest[1]+paid
   if not line.fish and line.rest[1]>.48 and #line.p<Budget.nodes then
    local a,b=line.p[1],line.p[2];local av,bv=line.prev[1],line.prev[2]
    local u=.24/line.rest[1]
    table.insert(line.p,2,{a[1]+(b[1]-a[1])*u,a[2]+(b[2]-a[2])*u,a[3]+(b[3]-a[3])*u})
    table.insert(line.prev,2,{av[1]+(bv[1]-av[1])*u,av[2]+(bv[2]-av[2])*u,av[3]+(bv[3]-av[3])*u})
    local total=line.rest[1];line.rest[1]=.24;table.insert(line.rest,2,total-.24)
   end
  end
 end
 line.p[1]=copy3(tip);line.prev[1]=copy3(tip)
 for i=1,#line.rest do line.lambda[i]=0 end
 local span_lambda=0
 if Physics.solve_rope then
  span_lambda=Physics.solve_rope(line,h,line.fish and 2 or Budget.iterations)
 else
 for iteration=1,(line.fish and 2 or Budget.iterations) do
  if iteration%2==1 then for i=1,#line.rest do constraint(line,i,h) end
  else for i=#line.rest,1,-1 do constraint(line,i,h) end end
  -- Coarse unilateral end-to-end constraint accelerates convergence across
  -- the large lure / line mass ratio, without making a slack rope push.
  if #line.rest>1 then
   local p=line.p[#line.p];local x,y,z=p[1]-tip[1],p[2]-tip[2],p[3]-tip[3]
   local distance=length3(x,y,z)
   if distance>line.paid then
    local w=1/line.mass;local alpha=line.paid/line.axial/(h*h)
    local dl=(-(distance-line.paid)-alpha*span_lambda)/(w+alpha)
    span_lambda=span_lambda+dl
    local k=w*dl/distance;p[1]=p[1]+k*x;p[2]=p[2]+k*y;p[3]=p[3]+k*z
   end
  end
  if not line.landed then
   for i=2,#line.p-1 do if line.p[i][2]<0 then line.p[i][2]=0 end end
  else
   local p=line.p[#line.p]
   if not line.retrieving_started then p[2]=line.surface and 0 or math.min(0,p[2]) end
  end
 end
 end
 -- Damp only relative motion, preserving bulk travel and the fly's large
 -- travelling loop. This removes unresolved particle-scale zigzags rather
 -- than repeatedly storing constraint corrections as spring velocity.
 if released or line.landed then
  if Physics.damp_rope then Physics.damp_rope(line,h) else
  local blend=1-math.exp(-(line.fly and 14 or 45)*h)
  local velocities=line.velocities or {};line.velocities=velocities
  local axial_blend=line.fish and 0 or (1-math.exp(-80*h))
  for i=1,#line.p do local p,v=line.p[i],line.prev[i]
   local velocity=velocities[i] or {};velocities[i]=velocity
   velocity[1]=(p[1]-v[1])/h;velocity[2]=(p[2]-v[2])/h;velocity[3]=(p[3]-v[3])/h
  end
  for i=2,#line.p-1 do
   for axis=1,3 do
    local v=velocities[i][axis]
    local mean=(velocities[i-1][axis]+velocities[i+1][axis])*.5
    line.prev[i][axis]=line.p[i][axis]-(v+(mean-v)*blend)*h
   end
  end
  -- Remove separating axial velocity on taut segments (inelastic tension).
  -- Mass weighting avoids damping the heavy lure as if it were a line node.
  for i=1,#line.rest do
   local a,b=line.p[i],line.p[i+1];local ap,bp=line.prev[i],line.prev[i+1]
   local x,y,z=b[1]-a[1],b[2]-a[2],b[3]-a[3]
   local d=length3(x,y,z)
   if d>=line.rest[i]*.998 and d>1e-8 then
    local speed=((b[1]-bp[1]-a[1]+ap[1])*x+(b[2]-bp[2]-a[2]+ap[2])*y+(b[3]-bp[3]-a[3]+ap[3])*z)/(h*d)
    if speed>0 then
     local wa=i==1 and 0 or 1/math.max(.00002,line.density*line.rest[i])
     local wb=i+1==#line.p and 1/line.mass or 1/math.max(.00002,line.density*line.rest[i])
     local impulse=speed*axial_blend/(wa+wb)*h/d
     local ia,ib=wa*impulse,wb*impulse
     ap[1]=ap[1]-ia*x;ap[2]=ap[2]-ia*y;ap[3]=ap[3]-ia*z
     bp[1]=bp[1]+ib*x;bp[2]=bp[2]+ib*y;bp[3]=bp[3]+ib*z
    end
   end
  end
 end
 end
 if line.fish and (line.fish.nearboat or line.fish.action~='jump') then
  local p,previous=line.p[#line.p],line.prev[#line.prev]
  if p[2]>-.06 then p[2]=-.06;previous[2]=math.min(previous[2],-.06) end
 end
 line.tension=math.min(line.fish and 500 or 8,(math.abs(line.lambda[1] or 0)+math.abs(span_lambda))/(h*h))
 end
 local endpoint=line.p[#line.p]
 if released and not line.landed and endpoint[2]<=0 then
  local prev=line.prev[#line.prev]
  local ivx,ivy,ivz=(endpoint[1]-prev[1])/h,(endpoint[2]-prev[2])/h,(endpoint[3]-prev[3])/h
  local u=clamp(prev[2]/math.max(.00001,prev[2]-endpoint[2]),0,1)
  endpoint[1]=prev[1]+u*(endpoint[1]-prev[1]);endpoint[3]=prev[3]+u*(endpoint[3]-prev[3]);endpoint[2]=0
  line.impact=copy3(endpoint);line.impact_speed=math.abs(ivy)
  line.landed=true;line.surface=lure.surface or lure.kind=='iso'
  local retained=line.surface and 0 or .12
  line.prev[#line.prev]={endpoint[1]-ivx*h*retained,endpoint[2]-ivy*h*retained,endpoint[3]-ivz*h*retained}
  line.payout=0
 end
end
local function smooth(a,b,t) t=clamp(t,0,1);t=t*t*t*(t*(t*6-15)+10);return a+(b-a)*t end
local function cast_motion(time,kind)
 if kind=='fly' then
  -- Pause for line turnover before each reversal. Final delivery opens the
  -- spool only after the forward loop has begun travelling away from the rod.
  local cycle=floor(time/1.8);local u=time%1.8
  if cycle>=3 then return -2.43,true end
  if u<.32 then return smooth(cycle==0 and -1.98 or -2.43,-1.56,u/.32),false
  elseif u<.90 then return -1.56,false
  elseif u<1.22 then return smooth(-1.56,-2.43,(u-.90)/.32),false
  else return -2.43,cycle==2 and u>=1.48 end
 elseif kind=='iso' then
  if time<.5 then return smooth(-1.98,-2.42,time/.5),false
  elseif time<.93 then return smooth(-2.42,-1.72,(time-.5)/.43),false
  else return smooth(-1.72,-2.45,(time-.93)/.36),time>=1.19 end
 end
 if time<.52 then return smooth(-1.98,-1.56,time/.52),false
 elseif time<.66 then return -1.56,false
 else return smooth(-1.56,-2.62,(time-.66)/.33),time>=.88 end
end
local function start_cast()
 if not reel_index or not lure_index then menu=true;tab=not reel_index and 2 or 3;return end
 reset_sim();sim.state='casting';sim.cast_clock=0;sim.accumulator=0;sim.phase='backswing'
 local wp=world_rod(sim.physics)
 sim.line=new_line(wp[65],equipped_lure(),rod.kind=='fly');sim.line.reel_kind=reels[reel_index].kind
 print('FISHING_CAST_START '..rod.model..' / '..reels[reel_index].model..' / '..equipped_lure().model)
end
local function release_cast(gesture,now)
 if not reel_index or not lure_index then menu=true;tab=not reel_index and 2 or 3;return end
 if rod.kind=='fly' and (gesture.reversals or 0)<2 then sim.state='ready';return end
 -- Manual drag remains available; single click uses the complete stroke above.
 start_cast();sim.strength=clamp(math.abs(gesture.velocity or 1000)/1400,.35,1.3)
end
-- Reel in material at the guide, removing consumed spans without scaling or
-- teleporting the remaining rope. The endpoint moves only through forces and XPBD.
local function wind_line(rope,amount)
 local minimum=rope.minimum or .65
 local take=math.min(math.max(0,amount),math.max(0,rope.paid-minimum))
 rope.paid=rope.paid-take
 while take>1e-10 do
  local available=rope.rest[1]
  if take>=available and #rope.rest>1 then
   take=take-available
   table.remove(rope.rest,1);table.remove(rope.p,2);table.remove(rope.prev,2)
  else rope.rest[1]=available-take;take=0 end
 end
 -- Merge a nearly consumed guide span; tiny rest lengths amplify solver noise.
 if #rope.rest>1 and rope.rest[1]<.025 then
  rope.rest[2]=rope.rest[2]+rope.rest[1]
  table.remove(rope.rest,1);table.remove(rope.p,2);table.remove(rope.prev,2)
 end
end
local function retrieve_click()
 if sim.state~='waiting' or not sim.line then return end
 local rope=sim.line
 rope.minimum=rod.kind=='iso' and .85 or .65
 if rope.paid<=rope.minimum+1e-7 then return end
 rope.retrieving_started=true
 sim.retrieve=sim.retrieve or {queue=0,clock=0,count=0}
 local r=sim.retrieve
 if rod.kind=='iso' then
  r.count=(r.last_click and sim.elapsed-r.last_click<=.65) and r.count+1 or 1
  r.last_click=sim.elapsed
  if r.count>=4 then r.full=true end
 end
 r.queue=math.min(3,r.queue+1)
end
local Surface={effects={}}
function Surface.emit(p,kind)
 if #Surface.effects>=8 then table.remove(Surface.effects,1) end
 Surface.effects[#Surface.effects+1]={p=copy3(p),kind=kind,time=sim.elapsed}
end
function Surface.stroke(rope,r,h,u)
 local lure=equipped_lure()
 if not lure or not lure.surface or sim.fish then return end
 local p,prev=rope.p[#rope.p],rope.prev[#rope.prev]
 if lure.icon==6 then
  -- Alternate lateral acceleration, perpendicular to the actual retrieval direction.
  local dx,dz=rope.p[1][1]-p[1],rope.p[1][3]-p[3]
  local n=math.max(.01,length3(dx,0,dz));local a=7*sin(pi*u)*(r.walk_side or 1)
  prev[1]=prev[1]-dz/n*a*h*h;prev[3]=prev[3]+dx/n*a*h*h
 elseif lure.icon==5 and not r.popped and u>=.10 then
  r.popped=true;Surface.emit(p,'pop')
 end
end
local function step_retrieve(h)
 local r=sim.retrieve
 if not r or sim.state~='waiting' then return 0 end
 local rope=sim.line
 if rope.paid<=rope.minimum+1e-7 then
  r.queue=0;r.full=false;r.active=false;sim.state='retrieved'
  print(string.format('FISHING_RETRIEVED %.3fm / %d nodes',rope.paid,#rope.p))
  return 0
 end
 if not r.active and (r.queue>0 or r.full) then
  r.queue=math.max(0,r.queue-1);r.active=true;r.clock=0
  r.walk_side=-(r.walk_side or -1);r.popped=false
  local iso=rod.kind=='iso'
  r.duration=iso and .48 or .58
  r.amount=iso and (r.full and .55 or .12) or (rod.kind=='fly' and .5 or .75)
  r.amplitude=iso and .025 or .095
 end
 if not r.active then return 0 end
 local before=r.clock/r.duration
 r.clock=math.min(r.duration,r.clock+h)
 local u=r.clock/r.duration
 Surface.stroke(rope,r,h,u)
 wind_line(rope,r.amount*(smooth(0,1,u)-smooth(0,1,before)))
 -- A bounded handle stroke excites the existing damped beam. Its response
 -- depends on length/action/power and the rope's computed tension, not noise.
 local jerk=r.amplitude*sin(pi*u)^2
 if r.clock>=r.duration then r.active=false end
 return jerk
end
-- Small synthesized event sounds; PCM is generated once, never during physics.
local sound_output,sound_pending
local sound_cache={}
local function queue_sound(kind) if sound_cache[kind] then sound_pending=sound_cache[kind] end end
local function init_fishing_audio()
 if fixed or A.check=='1' then return end
 local ok,audio=pcall(require,'audio')
 if not ok then print('FISHING_AUDIO unavailable');return end
 local output,err=audio.new_output({sample_rate=16000,channels=1,bits_per_sample=16,volume=35})
 if not output then print('FISHING_AUDIO '..tostring(err));return end
 sound_output=output
 local info=output:info();local rate=info.sample_rate or 16000
 local align=math.max(2,info.bytes_per_frame or 640)
 for _,sound in ipairs({{'bite',.045,380},{'hook',.07,650},{'drag',.018,2100},{'caught',.18,900},{'lost',.12,180},{'slap',.05,120}}) do
  local samples=math.floor(rate*sound[2]);local bytes={}
  for i=1,samples do
   local u=i/samples
   local frequency=sound[3]*(sound[1]=='caught' and (1+u*.5) or 1)
   local value=math.floor(sin(i*2*pi*frequency/rate)*6500*(1-u)^2)
   if value<0 then value=value+65536 end
   bytes[i]=string.char(value%256,math.floor(value/256))
  end
  local pcm=table.concat(bytes);local padding=(align-#pcm%align)%align
  sound_cache[sound[1]]=pcm..string.rep(string.char(0),padding)
 end
 print('FISHING_AUDIO ready')
end
local last_drag_sound=0
local function update_fishing_audio(now)
 if not sound_output then return end
 if sim.fish and sim.line and sim.line.payout>.05 and now-last_drag_sound>120 then
  if not sound_pending then queue_sound('drag') end;last_drag_sound=now
 end
 if sound_pending then
  local ok,err,written=sound_output:write(sound_pending)
  if ok then sound_pending=nil
  elseif written and written>0 then sound_pending=sound_pending:sub(written+1)
  elseif err~='audio output: busy' then sound_pending=nil end
 end
end
-- Coastal encounter / fight engine. All thresholds are desktop calibration.
local fish_enabled=A.check~='1'
local random_seed=math.floor(system.millis()%2147483646)+1
local function fish_random()
 random_seed=(random_seed*48271)%2147483647
 return random_seed/2147483647
end
local habitats={
 -- distance min/max, depth min/max, abundance, diet (predator / grazer / mixed)
 {1,24,1,12,7,'mixed'},{2,30,.1,9,8,'mixed'},{2,26,.1,7,8,'mixed'},
 {2,30,.1,5,6,'grazer'},{1,22,.1,4,7,'grazer'},{1,22,0,6,4,'mixed'},
 {3,45,0,5,12,'predator'},{10,55,1,18,3,'predator'},{8,55,0,8,4,'predator'},
 {15,65,0,10,4,'predator'},{18,70,0,14,1,'predator'},{35,100,0,5,1,'predator'},
 {2,35,0,7,6,'predator'},{1,25,1,8,7,'mixed'},{1,30,0,4,4,'grazer'},
 {1,24,.1,5,5,'grazer'},{10,55,1,20,4,'mixed'},{2,35,.2,10,6,'predator'},
 {12,60,1,18,3,'predator'},{2,40,1,14,6,'predator'},{10,55,2,20,2,'predator'},
 {15,70,3,25,.25,'predator'},{2,35,1,14,5,'predator'},{12,60,2,22,2,'predator'},
 {8,55,0,8,8,'predator'},{12,60,0,12,2,'predator'},{20,80,1,25,2,'predator'},
 {12,60,1,18,4,'predator'},{40,110,0,15,.5,'predator'},{25,85,0,12,2,'predator'},
}
local reefs={{-3,14,1.7},{3.5,23,2.2},{-7,31,2.5}}
local function bottom_depth(x,z) return 1.5+math.max(0,z-2)*.16+.3*sin(x*.8) end
-- Before hooking, the rope endpoint is the float, with a separate submerged
-- leader/hook. After hooking, the endpoint is the fish's mouth.
local FloatRig={}
function FloatRig.hook(rope,lure)
 local p=rope.p[#rope.p];local depth=lure.leader_m or .7
 return {p[1],math.max(-bottom_depth(p[1],p[3])+.08,p[2]-depth),p[3]}
end
function FloatRig.position(rope,lure)
 local p=rope.p[#rope.p]
 if not rope.fish then return copy3(p) end
 local a=rope.p[1];local distance=length3(a[1]-p[1],a[2]-p[2],a[3]-p[3])
 local u=math.min(1,(lure.leader_m or .7)/math.max(.01,distance))
 return {p[1]+(a[1]-p[1])*u,p[2]+(a[2]-p[2])*u,p[3]+(a[3]-p[3])*u}
end
local function nearby_reef(p)
 local best,dist=nil,1e9
 for _,r in ipairs(reefs) do local d=length3(p[1]-r[1],0,p[3]-r[2])
  if d<dist then best,dist=r,d end
 end
 return best,dist
end
local function habitat_weight(i,distance,depth,lure)
 local h=habitats[i]
 if distance<h[1] or distance>h[2] or depth<h[3] or depth>h[4] then return 0 end
 local diet=h[6]
 if lure.kind=='iso' then return h[5]*(diet=='predator' and .20 or 1) end
 if diet=='grazer' then return 0 end
 if diet=='mixed' then return h[5]*((lure.icon==3 or lure.kind=='fly') and .35 or .08) end
 return h[5]
end
local function pick_encounter(rope,lure)
 local p=lure.kind=='iso' and FloatRig.hook(rope,lure) or rope.p[#rope.p]
 local depth=math.max(0,-p[2])
 local distance=length3(p[1]-.915,0,p[3]-1.3)
 local total,baseline=0,0;local weights={}
 local environment=Weather.cache or Weather.sample((system.millis()-start)/1000)
 for i=1,#fish_types do
  local base=habitat_weight(i,distance,depth,lure)
  local weight=base*Weather.feeding_factor(i,environment)
  weights[i]=weight;total=total+weight;baseline=baseline+base
 end
 if total==0 or baseline==0 then return nil end
 -- One roll per scheduled encounter opportunity, never per render frame.
 -- Relative activity changes both encounter frequency and species composition.
 if fish_random()>1-math.exp(-.8*total/baseline) then return nil end
 local selected=fish_random()*total
 for i,w in ipairs(weights) do selected=selected-w
  if selected<=0 and w>0 then
   local species=fish_types[i]
   local kg=species.reference_min_kg+(species.reference_max_kg-species.reference_min_kg)*fish_random()^2
   kg=math.floor(kg*100+.5)/100
   local heading=fish_random()*2*pi;local radius=1+fish_random()*2.5
   local x,z=p[1]+cos(heading)*radius,math.max(1,p[3]+sin(heading)*radius)
   local bed=-bottom_depth(x,z)+.15
   local y=clamp(p[2]-.2-fish_random()*1.8,bed,-.12)
   return {species=species,index=i,kg=kg,cm=math.max(6,math.floor(species.cm*(kg/species.kg)^(1/3)+.5)),
    surface_attack=lure.kind~='iso' and lure.surface==true,phase='approach',clock=0,position={x,y,z},previous={x,y,z},bite_side=fish_random()<.5 and -1 or 1}
  end
 end
end
local function rig_line_setup()
 -- Explicit installed line presets; capacity markings do not imply strength.
 local reel=reels[reel_index];local kind=reel and reel.kind or 'spin'
 local preset=kind=='fly' and {30,6} or (kind=='round' and {100,22} or (kind=='bait' and {65,15} or {50,12}))
 local max_drag=reel and tonumber(reel.spec:match('DRAG%s*([%d%.]+)KG')) or 4
 return {strength=preset[1],drag=math.min(preset[2],(max_drag or 4)*9.81),capacity=100}
end
local function start_hook()
 local e=sim.encounter
 if not e or e.phase~='take' or sim.state~='waiting' then return false end
 local rope=sim.line;local b=e.species.behavior
 local setup=rig_line_setup()
 sim.state='fight';sim.retrieve=nil;sim.encounter=nil
 e.energy=1;e.age=0;e.action=e.surface_attack and 'dive' or 'recover';e.action_clock=0;e.action_duration=e.surface_attack and .9 or .65
 e.cooldown=0;e.slack_risk=0;e.hook_damage=0;e.line_health=1;e.heading=1;e.side=0
 e.queue=0;e.wind_clock=0;e.wind_active=false;e.safe_time=0;e.draw_angle=0
 e.start_angle=sim.physics.angle;e.break_stress=0
 e.quality=clamp(.92-math.abs(e.clock-.55)*.25,.60,.94)
 e.hook_limit=setup.drag*(.78+b.hook_hold*.007)*e.quality
 sim.fish=e;sim.hook_clock=0;sim.line_setup=setup;queue_sound('hook')
 local endpoint,previous=copy3(rope.p[#rope.p]),copy3(rope.prev[#rope.prev])
 local lure=equipped_lure()
 if lure.kind=='iso' then
  local hook=FloatRig.hook(rope,lure)
  previous[2]=previous[2]+hook[2]-endpoint[2];endpoint=hook
  rope.paid=rope.paid+(lure.leader_m or .7)
 end
 rope.p={copy3(rope.p[1]),endpoint};rope.prev={copy3(rope.prev[1]),previous}
 -- Hook-set takes up the existing slack before loading the installed line.
 rope.paid=math.min(rope.paid,length3(endpoint[1]-rope.p[1][1],endpoint[2]-rope.p[1][2],endpoint[3]-rope.p[1][3]))
 rope.rest={rope.paid};rope.lambda={0};rope.velocities=nil
 rope.mass=e.kg;rope.fish=e;rope.surface=false;rope.minimum=.85;rope.retrieving_started=true
 rope.water={mass=e.kg,acceleration=0,drag=.5*1025*.005*e.kg^(2/3),surface=false}
 print(string.format('FISHING_HOOKED %s %.2fkg / drag %.1fN',e.species.name,e.kg,setup.drag))
 return true
end
local function detach_fish(reason)
 local f=sim.fish;if not f then return end
 print(string.format('FISHING_LOST %s / %s / tension %.2f payout %.2f paid %.2f age %.2f',reason,f.species.name,sim.line.tension,sim.line.payout,sim.line.paid,f.age))
 queue_sound('lost');sim.last_loss=reason;sim.fish=nil;sim.encounter=nil;sim.physics.fight_dir=nil;sim.physics.yaw=0;sim.physics.rail_blend=nil
 sim.state='waiting';sim.encounter_clock=0;sim.next_encounter=9
 local lure=equipped_lure();local rope=sim.line
 if lure.kind=='iso' then
  local float=FloatRig.position(rope,lure)
  rope.p[#rope.p]=float;rope.prev[#rope.prev]=copy3(float)
  rope.paid=math.max(.65,rope.paid-(lure.leader_m or .7));rope.rest={rope.paid};rope.lambda={0}
 end
 rope.fish=nil;rope.mass=math.max(.001,lure.weight/1000);rope.water=water_profile(lure)
 rope.surface=lure.surface or lure.kind=='iso';rope.minimum=.65
 if reason=='LINE BREAK' or reason=='REEF ABRASION' then
  sim.state='lost';sim.line=nil;sim.loss_clock=0
 end
end
local function complete_catch()
 local f=sim.fish;if not f or f.recorded or sim.state~='landing' then return end
 f.recorded=true;queue_sound('caught')
 local record={};for k,v in pairs(f.species) do record[k]=v end
 record.kg=f.kg;record.cm=f.cm;record.catch_id=#catches+1;record.caught_ms=system.millis();record.location='COASTAL REEF'
 catches[#catches+1]=record;focus[4]=#catches
 scrolls[4]=clamp(floor((#catches-1)/3)*96,0,max_scroll(4))
 sim.state='landed';sim.landing_clock=0;sim.landed_record=record
 sim.fish=nil;sim.line=nil;sim.physics.fight_dir=nil;sim.physics.yaw=0
 print(string.format('FISHING_CAUGHT %s %.2fkg %d coins / bag %d',record.name,record.kg,fish_value(record),#catches))
end
local function fight_click(x)
 local f=sim.fish
 if not f then return end
 if sim.state=='landing' and x>=120 and x<248 then complete_catch();return end
 if x<120 then f.side=clamp(f.side-.18,-.48,.48)
 elseif x>=248 then f.side=clamp(f.side+.18,-.48,.48)
 else f.queue=math.min(2,f.queue+1) end
end
local function choose_fish_action(f,p)
 local reef,distance=nearby_reef(p)
 local weights=fish_behavior_weights(f.species,f.kg,f.energy,distance<7,p[2]>-.35,f.cooldown,f.action)
 local roll=fish_random();local action='recover'
 for _,key in ipairs({'run','turn','shake','dive','jump','recover'}) do
  roll=roll-weights[key];if roll<=0 then action=key;break end
 end
 f.action=action;f.action_clock=0;f.heading=fish_random()<.5 and -1 or 1
 f.action_duration=action=='run' and f.species.behavior.sprint_seconds or (1.0+fish_random()*1.7)
 if action=='run' then f.cooldown=f.action_duration+f.species.behavior.sprint_cooldown end
 f.reef=reef
 print(string.format('FISHING_ACTION %s / %s / energy %.2f',f.species.short,action,f.energy))
end
local function step_encounter(h,lure)
 if not fish_enabled or sim.state~='waiting' then return 0 end
 sim.encounter_clock=(sim.encounter_clock or 0)+h
 if not sim.next_encounter then sim.next_encounter=4+fish_random()*4 end
 if not sim.encounter and sim.encounter_clock>=sim.next_encounter and sim.line.paid>1.5 then
  sim.encounter=pick_encounter(sim.line,lure)
  sim.next_encounter=sim.encounter_clock+5+fish_random()*5
 end
 local e=sim.encounter;if not e then return 0 end
 e.clock=e.clock+h
 local p=sim.line.p[#sim.line.p]
 local hook=lure.kind=='iso' and FloatRig.hook(sim.line,lure) or p
 local bed=-bottom_depth(p[1],p[3])+.04
 if p[2]<bed then p[2]=bed;sim.line.prev[#sim.line.prev][2]=bed end
 e.previous=e.previous or copy3(e.position)
 for axis=1,3 do
  e.previous[axis]=e.position[axis]
  local target=hook[axis]
  if axis==2 then target=e.surface_attack and e.phase~='approach' and p[axis] or math.min(-.12,hook[axis]-(lure.kind=='iso' and 0 or .12)) end
  e.position[axis]=e.position[axis]+(target-e.position[axis])*math.min(1,h*1.5)
 end
 e.position[2]=math.max(-bottom_depth(e.position[1],e.position[3])+.12,e.position[2])
 if e.phase=='approach' then
  if e.clock>1.2+e.species.behavior.caution*.006 then e.phase='peck';e.clock=0;e.rise_y=e.position[2] end
  return 0
 elseif e.phase=='peck' then
  if e.surface_attack then e.position[2]=e.rise_y*(1-smooth(0,1,e.clock/.65)) end
  if e.clock>.65 then
   if e.surface_attack then Surface.emit(p,'attack');sim.line.surface=false end e.phase='take';e.clock=0;print('FISHING_BITE '..e.species.short);queue_sound('bite') end
  return .18*math.max(0,sin(e.clock*20))
 end
 -- A committed bite actually carries the rig sideways, producing line motion.
 local prev=sim.line.prev[#sim.line.prev]
 prev[1]=prev[1]-e.bite_side*.8*h*h
 if lure.kind=='iso' or e.surface_attack then sim.line.surface=false;prev[2]=prev[2]+2.8*h*h end
 if e.clock>1.65 then sim.encounter=nil;sim.line.surface=lure.surface or lure.kind=='iso';return 0 end
 return .65+.3*sin(e.clock*12)^2
end
local function step_fish_before(h)
 local f=sim.fish;if not f then return 0 end
 local rope=sim.line;local p,prev=rope.p[#rope.p],rope.prev[#rope.prev]
 f.age=f.age+h;f.action_clock=f.action_clock+h;f.cooldown=math.max(0,f.cooldown-h)
 sim.hook_clock=sim.hook_clock+h
 if f.action_clock>=f.action_duration then choose_fish_action(f,p) end
 local b=f.species.behavior
 f.nearboat=length3(p[1]-1.05,0,p[3]-1.85)<3.5
 local blend=sim.physics.rail_blend or 0
 sim.physics.rail_blend=clamp(blend+(f.nearboat and h*.8 or -h*.8),0,1)
 if f.nearboat then
  local tip=rope.p[1]
  -- Retain the actual rod-to-mouth leader; do not manufacture slack by
  -- stopping at a guessed distance to the rail.
  rope.minimum=math.max(.85,length3(tip[1]-p[1],tip[2]-p[2],tip[3]-p[3])-.005)
  if f.action=='jump' then f.action='recover' end
 end
 local rate=(f.action=='run' and .075 or (.005+(100-b.endurance)*.00025))+(rope.tension or 0)/math.max(.1,f.kg)*.0015
 if f.action=='recover' then rate=rate*.35 end
 f.energy=clamp(f.energy-rate*h+(f.action=='recover' and b.recovery*.00009*h or 0),.04,1)
 local x,y,z=0,0,0
 local speed=(.18+b.burst*.009)*clamp(f.kg^.12,.6,1.7)*(.22+.78*f.energy)
 if f.action=='run' then x=f.heading*.5;z=1;speed=speed*2.2
 elseif f.action=='turn' then x=f.heading;z=.12
 elseif f.action=='shake' then x=f.heading*.3;z=.12;speed=speed*.45
 elseif f.action=='dive' then
  y=-.7;z=.3
  if f.reef then x=(f.reef[1]-p[1])*.25;z=(f.reef[2]-p[3])*.25 end
 elseif f.action=='jump' then y=f.action_clock<.3 and 1 or -.2;x=f.heading*.3;speed=speed*1.4
 else
  if f.nearboat then x=1.05-p[1];z=1.85-p[3];y=.18 else x=f.heading*.12;z=-.2 end
  speed=speed*.3
 end
  local norm=length3(x,y,z);if norm>.001 then x,y,z=x/norm,y/norm,z/norm end
 local vx,vy,vz=(p[1]-prev[1])/h,(p[2]-prev[2])/h,(p[3]-prev[3])/h
 local response=1.0+b.agility*.015
 local ax,ay,az=(x*speed-vx)*response,(y*speed-vy)*response,(z*speed-vz)*response
 local accel=length3(ax,ay,az);local cap=(.8+b.burst*.028)*(.3+.7*f.energy)
 if accel>cap then ax,ay,az=ax*cap/accel,ay*cap/accel,az*cap/accel end
 if f.action=='shake' then ax=ax+sin(f.action_clock*26)*(.2+b.headshake*.008) end
 prev[1]=prev[1]-ax*h*h;prev[2]=prev[2]-ay*h*h;prev[3]=prev[3]-az*h*h
 local bed=-bottom_depth(p[1],p[3])+.12
 if p[2]<bed then p[2]=bed;prev[2]=bed end
 if p[3]<1 then p[3]=1;prev[3]=math.max(prev[3],1) end
 if not f.wind_active and f.queue>0 then f.queue=f.queue-1;f.wind_active=true;f.wind_clock=0 end
 if f.wind_active then
  local before=f.wind_clock/.55;f.wind_clock=math.min(.55,f.wind_clock+h)
  if rope.tension<sim.line_setup.drag*.9 then
   wind_line(rope,.38*(smooth(0,1,f.wind_clock/.55)-smooth(0,1,before)))
  end
  if f.wind_clock>=.55 then f.wind_active=false end
 end
 f.side=f.side*math.exp(-h*.16)
 sim.physics.yaw=(sim.physics.yaw or 0)+clamp(f.side-(sim.physics.yaw or 0),-h*.8,h*.8)
 local hook=sim.hook_clock<.5 and .06*sin(pi*sim.hook_clock/.5)^2 or 0
 return hook+(f.wind_active and .04*sin(pi*f.wind_clock/.55) or 0)
end
local function step_fish_after(h,tip)
 local f=sim.fish;if not f then return end
 local rope=sim.line;local p,prev=rope.p[#rope.p],rope.prev[#rope.prev]
 local dx,dy,dz=p[1]-tip[1],p[2]-tip[2],p[3]-tip[3]
 local distance=length3(dx,dy,dz)
 if distance>.001 then sim.physics.fight_dir={dx/distance,dy/distance,dz/distance} end
 local tension=rope.tension;local speed=length3(p[1]-prev[1],p[2]-prev[2],p[3]-prev[3])/h
 if tension<.12 and rope.paid-distance>.15 then
  f.slack_risk=f.slack_risk+h*(f.action=='shake' and 1.7 or .5)
 else f.slack_risk=math.max(0,f.slack_risk-h*.5) end
 f.hook_damage=f.hook_damage+math.max(0,tension-f.hook_limit)/math.max(1,f.hook_limit)*h*.65
 -- Test the actual submerged line path against reef columns at 20 Hz.
 f.reef_clock=(f.reef_clock or 0)+h
 if f.reef_clock>=.05 then
  local elapsed=f.reef_clock;f.reef_clock=0
  local contact=false
  for _,reef in ipairs(reefs) do
   local top=-bottom_depth(reef[1],reef[2])+1.0
   for i=1,#rope.p-1 do
    local a,b=rope.p[i],rope.p[i+1]
    if math.min(a[2],b[2])<top then
     local x,z=b[1]-a[1],b[3]-a[3]
     local u=clamp(((reef[1]-a[1])*x+(reef[2]-a[3])*z)/math.max(.00001,x*x+z*z),0,1)
     local dx,dz=a[1]+u*x-reef[1],a[3]+u*z-reef[2]
     if dx*dx+dz*dz<reef[3]^2 and a[2]+u*(b[2]-a[2])<top then contact=true;break end
    end
   end
   if contact then break end
  end
  if contact then f.line_health=f.line_health-elapsed*(.05+.025*speed)*tension/sim.line_setup.drag end
 end
 local strength=sim.line_setup.strength*math.max(.2,f.line_health)
 f.break_stress=math.max(0,f.break_stress+h*(tension>strength and (tension-strength)/strength or -.5))
 if f.break_stress>.08 then detach_fish('LINE BREAK');return end
 if f.line_health<=.05 then detach_fish('REEF ABRASION');return end
 if f.hook_damage>1 then detach_fish('HOOK TEAR');return end
 if f.slack_risk>1.1+f.quality*1.8 then detach_fish('SLACK LINE');return end
 local screen=project3(p)
 local rail_distance=length3(p[1]-1.05,0,p[3]-1.85)
 if rail_distance<.65 and p[2]>-.18 and screen[2]>=400 and screen[1]>=260 and screen[1]<368 then
  sim.state='lifting';sim.lift_clock=0;sim.lift_position=copy3(p)
 end
end

local function tick_sim(dt)
 if sim.rod~=rod or sim.reel_index~=reel_index or sim.lure_index~=lure_index or not sim.physics then reset_sim() end
 if menu then return end
 if sim.state=='lifting' then
  sim.lift_clock=math.min(.45,sim.lift_clock+math.max(0,dt));sim.elapsed=sim.elapsed+math.max(0,dt)
  local p=sim.line.p[#sim.line.p];local previous=sim.line.prev[#sim.line.prev]
  previous[1],previous[2],previous[3]=p[1],p[2],p[3]
  p[2]=sim.lift_position[2]+.9*smooth(0,1,sim.lift_clock/.45)
  local tip=sim.line.p[1];sim.line.paid=length3(p[1]-tip[1],p[2]-tip[2],p[3]-tip[3]);sim.line.rest[1]=sim.line.paid
  sim.accumulator=0
  if sim.lift_clock>=.45 then sim.state='landing';complete_catch() end
  return
 end
 if sim.state=='landed' then
  local before=sim.landing_clock;sim.landing_clock=math.min(1.8,before+math.max(0,dt))
  for _,time in ipairs({.35,.95,1.50}) do if before<time and sim.landing_clock>=time then queue_sound('slap') end end
  sim.accumulator=0;return
 end
 local lure=equipped_lure()
 sim.accumulator=(sim.accumulator or 0)+math.max(0,dt)
 local h=CastWorld.h
 while sim.accumulator+1e-10>=h do
  sim.accumulator=sim.accumulator-h;sim.elapsed=sim.elapsed+h
  if sim.state=='landed' then
   sim.landing_clock=sim.landing_clock+h
   sim.accumulator=0;return
  elseif sim.state=='lost' then
   sim.loss_clock=sim.loss_clock+h
   if sim.loss_clock>1 then reset_sim() end
  end
  local target=sim.state=='aiming' and (sim.aim_angle or -1.98) or -1.98
  local releasing=false
  if sim.state=='casting' then
   sim.cast_clock=sim.cast_clock+h*(sim.strength or 1)
   target,releasing=cast_motion(sim.cast_clock,rod.kind)
   sim.phase=rod.kind=='fly' and 'false-cast' or (sim.cast_clock<.66 and 'backswing' or 'forward')
  elseif sim.state=='flight' or sim.state=='waiting' or sim.state=='retrieved' then
   target=smooth(sim.release_angle or -2.55,-2.62,sim.elapsed/.75)
  end
  local bite_load=step_encounter(h,lure)
  if sim.fish then
   local motion=step_fish_before(h)
   target=smooth(sim.fish.start_angle,sim.fish.nearboat and -2.40 or -2.08,sim.hook_clock/.7)+motion
  end
  target=target+step_retrieve(h)
  local reel=reels[reel_index]
  local drag_kg=reel and tonumber(reel.spec:match('DRAG%s*([%d%.]+)KG')) or 5
  RodPhysics.step(sim.physics,h,{handle_angle=target,lure_g=sim.state=='casting' and lure and lure.weight or 0,
   line_n=(sim.line and math.min(sim.fish and 40 or .7,sim.line.tension) or 0)+bite_load,
   fish_kg=0,drag_n=drag_kg*9.81})
  if sim.line then
   local tip=world_rod(sim.physics,true)
   step_line(sim.line,tip,h,sim.state=='flight' or sim.state=='waiting' or sim.state=='retrieved' or sim.fish~=nil,lure)
   if sim.fish then step_fish_after(h,tip);if sim.state=='landed' or sim.state=='lifting' then sim.accumulator=0;return end end
   if releasing and sim.state=='casting' then
    sim.state='flight';sim.phase='flight';sim.elapsed=0;sim.release_angle=sim.physics.angle
    sim.launch=copy3(sim.line.p[#sim.line.p]);sim.release_tip=copy3(tip)
    local p,prev=sim.line.p[#sim.line.p],sim.line.prev[#sim.line.prev]
    sim.release_speed=length3(p[1]-prev[1],p[2]-prev[2],p[3]-prev[3])/h
    print(string.format('FISHING_RELEASE %.2fm/s velocity=(%.2f,%.2f,%.2f) / %.3f bend / %d nodes',sim.release_speed,(p[1]-prev[1])/h,(p[2]-prev[2])/h,(p[3]-prev[3])/h,sim.physics.q,#sim.line.p))
   elseif sim.state=='flight' and sim.line.landed then
    sim.state='waiting';sim.phase='splash';sim.elapsed=0
    local p=sim.line.impact
    sim.range=length3(p[1]-.915,0,p[3]-1.3)
    print(string.format('FISHING_LANDED %.2fm / %.2fm line / %d nodes',sim.range,sim.line.paid,#sim.line.p))
   end
  end
 end
end
local function world_line(a,b,color)
 if a[3]<CastWorld.near and b[3]<CastWorld.near then return end
 if a[3]<CastWorld.near or b[3]<CastWorld.near then
  local t=(CastWorld.near-a[3])/(b[3]-a[3]);local cut={a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t,CastWorld.near}
  if a[3]<CastWorld.near then a=cut else b=cut end
 end
 local p,q=project3(a),project3(b)
 -- Clip in screen space before passing coordinates to the native line API.
 local dx,dy=q[1]-p[1],q[2]-p[2];local lo,hi=0,1
 for _,edge in ipairs({{-dx,p[1]},{dx,367-p[1]},{-dy,p[2]},{dy,447-p[2]}}) do
  if math.abs(edge[1])<1e-8 then if edge[2]<0 then return end
  else local u=edge[2]/edge[1];if edge[1]<0 then lo=math.max(lo,u) else hi=math.min(hi,u) end end
 end
 if hi>=lo then line(p[1]+lo*dx,p[2]+lo*dy,p[1]+hi*dx,p[2]+hi*dy,color) end
end
function Surface.draw()
 if sim.state=='casting' then Surface.effects={};return end
 for i=#Surface.effects,1,-1 do
  local e=Surface.effects[i];local age=sim.elapsed-e.time
  if age<0 or age>.65 then table.remove(Surface.effects,i)
  else
   local strength=e.kind=='attack' and 1.8 or 1
   for j=1,8 do
    local a=j*2.399;local speed=(.25+j%3*.09)*strength
    local height=math.max(0,(.9+j%2*.3)*strength*age-4.905*age*age)
    local p={e.p[1]+cos(a)*speed*age,height,e.p[3]+sin(a)*speed*age}
    local q=project3(p);rect(q[1],q[2],e.kind=='attack' and 2 or 1,2,C.foam)
   end
   local radius=.03+age*.45*strength;local previous
   for j=0,12 do local a=j*pi/6;local p={e.p[1]+cos(a)*radius,.004,e.p[3]+sin(a)*radius}
    if previous then world_line(previous,p,mix(C.foam,C.sea1,age/.65)) end;previous=p
   end
  end
 end
end
local function draw_water_fish(f,p,previous)
 if p[3]<=CastWorld.near then return end
 local point,zoom=project3(p)
 local size=clamp(zoom*f.cm/100/58,.06,2.1)
 local angle=f.draw_angle or 0
 if previous then
  local old=project3(previous);local dx,dy=point[1]-old[1],point[2]-old[2]
  if dx*dx+dy*dy>1e-12 then
   local target=math.atan(dy,dx)-pi
   local delta=(target-angle+pi)%(2*pi)-pi
   angle=angle+delta*.12;f.draw_angle=angle
  end
 end
 local x=point[1]+23*cos(angle)*size;local y=point[2]+23*sin(angle)*size
 local tint
 if p[2]<=0 then tint=mix(rgb(0x10557e),C.sea2,clamp(-p[2]/8,0,.94)) end
 fish_icon(f.species,x,y,size,0,angle,tint)
 if f.nearboat and sim.state~='lifting' and p[2]>-.15 then
  -- Just the small leading part of the head breaks the surface; the body stays tinted.
  local surface=project3({p[1],0,p[3]})
  local ca,sa=cos(angle),sin(angle);local reach=math.max(3,size*7)
  poly({{point[1],surface[2]},{point[1]+ca*reach, surface[2]+sa*reach-2},
   {point[1]+ca*reach, surface[2]+sa*reach+2}},rgb(f.species.body))
 end
 if p[2]>=0 then
  for i=1,3 do line(x-12-i*3,y+7+i*2,x-5-i*3,y+7+i*2,C.bluefoam) end
 end
end
local deck_commands,deck_was_visible,deck_result_visible
local function deck_pose(t)
 local p={u=7,v=-5,angle=.10,flex=.3,height=0,splash=0}
 if t<.35 then
  local u=clamp(t/.35,0,1);p.u=-12+6*u;p.v=0;p.angle=.3-.2*u;p.flex=-1.5
  p.height=80*(1-u*u)
 elseif t<.50 then
  p.u=-6;p.v=0;p.flex=-1;p.splash=1-(t-.35)/.15
 elseif t<.95 then
  local u=(t-.50)/.45;p.u=-6+8*u;p.v=-4*u
  p.height=52*4*u*(1-u);p.angle=.1-.2*sin(pi*u);p.flex=-8*sin(pi*u);p.splash=.6*(1-u)
 elseif t<1.15 then
  p.u=2;p.v=-4;p.flex=1;p.splash=.8*(1-(t-.95)/.20)
 elseif t<1.50 then
  local u=(t-1.15)/.35;p.u=2+5*u;p.v=-4-u
  p.height=27*4*u*(1-u);p.angle=.1+.1*sin(pi*u);p.flex=6*sin(pi*u);p.splash=.3*(1-u)
 elseif t<1.8 then p.splash=.25*(1-(t-1.50)/.3) end
 return p
end
local function draw_deck_result(fish,time)
 if deck_was_visible and deck_result_visible and time>=1.8 then return end
 local pose=deck_pose(time)
 -- Shared oblique perspective for deck, fish, wet marks and cast shadow.
 -- Upper-left sunlight; distant boards narrow toward their vanishing points.
 local function deck(u,v,z)
  local x=u*.866+v*.5
  local depth=-u*.5+v*.866
  local perspective=1/(1+depth/1300)
  return 174+x*perspective,259-depth*.67*perspective-(z or 0)*perspective
 end
 local function patch(points,color)
  local q={};for _,p in ipairs(points) do local x,y=deck(p[1],p[2]);q[#q+1]={x,y} end;poly(q,color)
 end
 local function seam(u,v,x,y,color)
  local ax,ay=deck(u,v);local bx,by=deck(x,y)
  local dx,dy=bx-ax,by-ay;local lo,hi=0,1
  for _,edge in ipairs({{-dx,ax},{dx,367-ax},{-dy,ay},{dy,447-ay}}) do
   local p,q=edge[1],edge[2]
   if p==0 then if q<0 then return end
   elseif p<0 then lo=math.max(lo,q/p)
   else hi=math.min(hi,q/p) end
   if lo>hi then return end
  end
  line(ax+dx*lo,ay+dy*lo,ax+dx*hi,ay+dy*hi,color)
 end
 if not deck_commands then
  capture_commands={}
 rect(0,0,368,448,rgb(0xd6ae72))
 for row=-12,12 do
  local v=row*54
  local t=(row%4)*3
  patch({{-620,v},{620,v},{620,v+53},{-620,v+53}},rgb(0xcba16b+t*0x010101))
  seam(-620,v,620,v,rgb(0x937047))
  seam(-620,v+2,620,v+2,rgb(0xf3d6a0))
  for col=-4,4 do
   local u=col*184+(row%3)*61
   seam(u,v+3,u,v+52,rgb(0xa17c50))
   for k=1,3 do
    local x=u+15+k*9;local y=v+10+k*9
    seam(x,y,x+58+(col%3)*13,y,rgb(0xbc905c))
    seam(x+5,y+1,x+32,y+1,rgb(0xe1ba80))
   end
  end
 end
  deck_commands=capture_commands;capture_commands=nil
 end
 local full=not deck_was_visible or (time>=1.8 and not deck_result_visible)
 local top,bottom=full and 0 or 110,full and 448 or 350
 for _,c in ipairs(deck_commands) do
  if c[1]==0 then
   local y=math.max(top,c[3]);local finish=math.min(bottom,c[3]+c[5])
   if y<finish then rect(c[2],y,c[4],finish-y,c[6]) end
  elseif math.max(c[3],c[5])>=top and math.min(c[3],c[5])<bottom then
   local x,y,xx,yy=c[2],c[3],c[4],c[5]
   if y~=yy then
    local lo,hi=math.min((top-y)/(yy-y),(bottom-1-y)/(yy-y)),math.max((top-y)/(yy-y),(bottom-1-y)/(yy-y))
    lo,hi=math.max(0,lo),math.min(1,hi)
    if lo<=hi then line(x+(xx-x)*lo,y+(yy-y)*lo,x+(xx-x)*hi,y+(yy-y)*hi,c[6]) end
   elseif y>=top and y<bottom then line(x,y,xx,yy,c[6]) end
  end
 end
 deck_was_visible=true;deck_result_visible=time>=1.8
 local p=pose
 if time>=.35 then
  local wet={}
  for i=0,23 do local a=i*pi/12;wet[#wet+1]={12+cos(a)*(110+i%3*5),-4+sin(a)*40} end
  patch(wet,rgb(0xad9b76))
  for i=1,18 do
   local a=i*2.399;local u=10+cos(a)*(100+i%4*14);local v=sin(a)*57
   seam(u,v,u+3+i%3,v,rgb(0xd7ebde))
  end
 end
 -- Shadow displacement grows with height, away from the upper-left sun.
 local function shadow(u,v) local x,y=deck(u,v);return x+10+p.height*.45,y+12+p.height*.30 end
 fish_icon(fish,p.u,p.v,5.25,time*6,p.angle,rgb(0x927647),p.flex,shadow)
 if p.splash>0 then
  for i=1,22 do
   local a=i*2.399;local spread=100+(i%5)*13
   local x,y=deck(p.u+cos(a)*spread,p.v+sin(a)*50,(i%4)*12*p.splash)
   rect(x,y,3,3,rgb(0xf7ffff))
   if i%3==0 then line(x,y+4,x+2,y+8,rgb(0x8bced2)) end
  end
 end
 -- A small darker near-side thickness and sunlit flank establish volume.
 local function underside(u,v) return deck(u,v,p.height+1) end
 local function topside(u,v) return deck(u,v,p.height+7) end
 fish_icon(fish,p.u,p.v,5.25,time*6,p.angle,rgb(0x476a69),p.flex,underside)
 local lit={};for k,v in pairs(fish) do lit[k]=v end
 local function sun(hex)
  local c=mix(rgb(hex),rgb(0xfff4d6),.24)
  return c.r*65536+c.g*256+c.b
 end
 lit.body=sun(fish.body);lit.back=sun(fish.back);lit.belly=sun(fish.belly);lit.fin=sun(fish.fin)
 fish_icon(lit,p.u,p.v,5.25,time*6,p.angle,nil,p.flex,topside)
 for i=1,5 do
  local x,y=topside(p.u+(-13+i*5)*5.25,p.v-9)
  line(x,y,x+4,y,rgb(0xf4fae3))
 end
 if time>=1.8 then
  centered(0,34,368,fish.name,rgb(0x483e2e),math.min(2,344/(#fish.name*6)))
  centered(0,78,368,'CATCH LANDED',rgb(0x716046),1)
  rect(25,350,318,1,rgb(0x9d7b50))
  centered(20,374,104,tostring(fish.cm)..' CM',rgb(0x483e2e),2)
  centered(127,374,114,string.format('%.2f KG',fish.kg),rgb(0x483e2e),2)
  centered(247,374,104,tostring(fish_value(fish)),rgb(0x483e2e),2)
  centered(247,405,104,'COINS',rgb(0x716046),1)
 end
end
function FloatRig.draw(rope,lure,t)
 local p=FloatRig.position(rope,lure)
 if p[3]<=CastWorld.near then return end
 local screen,zoom=project3(p)
 -- Float diameter is 8 icon units, unlike a 34-unit hard bait. Keep its
 -- orange cap readable at distance rather than shrinking below one pixel.
 local size=clamp(zoom*.045/8*1.35,.75,1.4)
 if not rope.fish then
  local hook=FloatRig.hook(rope,lure)
  local wet=mix(C.foam,C.sea2,.78)
  if rope.landed then
   world_line(p,hook,wet)
   local q=project3(hook)
   ellipse(q[1],q[2],math.max(.7,size*1.5),math.max(.5,size),mix(C.sea2,rgb(0x102e39),.45))
  else world_line(p,hook,mix(C.cream,C.sea1,.35)) end
 end
 local x,y=screen[1],screen[2]
 if x< -20 or x>388 or y< -20 or y>468 then return end
 if not rope.landed then
  lure_icon(7,x,y,size,false,0,lure)
 elseif p[2]<-.06 then
  -- A taken float disappears under water; it is never rendered as bait.
  ellipse(x,y,math.max(1,size*3),math.max(1,size*4),mix(rgb(0x775b3e),C.sea2,clamp(.5-p[2]*.4,.5,.95)))
 else
  local bob=sin(t*2.5)*size*.35
  local peck=sim.encounter and sim.encounter.phase=='peck' and sin(sim.encounter.clock*20)*size*.6 or 0
  y=y+bob+peck
  ellipse(x,y+size,size*3,size*2,mix(C.sea2,rgb(0x593d29),.4))
  poly({{x-3*size,y},{x-2*size,y-4*size},{x,y-6*size},{x+2*size,y-4*size},{x+3*size,y}},rgb(0xff7334))
  line(x-size,y-3*size,x-size,y-size,rgb(0xffebb2))
  line(x-3*size,y,x+3*size,y,C.cream)
  line(x-5*size,y+2*size,x+5*size,y+2*size,mix(C.foam,C.sea1,.55))
 end
end
local function draw_live(t)
 if sim.state=='landed' and sim.landed_record then draw_deck_result(sim.landed_record,sim.landing_clock);return end
 deck_was_visible=false;deck_result_visible=false
 if sim.rod~=rod or sim.reel_index~=reel_index or sim.lure_index~=lure_index or not sim.physics then reset_sim() end
 local reel=reels[reel_index]
 local profile_start=system.millis()
 local wp,pts=world_rod(sim.physics)
 local profile_pose=system.millis()
 sea(t)
 local profile_sea=system.millis()
 for _,reef in ipairs(reefs) do
  local point,zoom=project3({reef[1],-bottom_depth(reef[1],reef[2])+1,reef[2]})
  ellipse(point[1],point[2],reef[3]*zoom,reef[3]*zoom*.22,rgb(0x0568b6))
 end
 local lure=equipped_lure()
 if lure and not sim.line and sim.state~='landed' and sim.state~='lost' then sim.line=new_line(wp[65],lure,false) end
 if sim.line then
  local rope=sim.line
  for i=#rope.p-1,1,-1 do
   local a,b=rope.p[i],rope.p[i+1]
   local depth=(a[3]+b[3])*.5
   local air=rope.fly and C.cream or mix(C.cream,C.sea1,clamp((depth-2)/45,0,.5))
   local wet=mix(C.foam,C.sea2,clamp(.55-math.min(a[2],b[2])*.055,.55,.94))
   if a[2]<0 and b[2]<0 then world_line(a,b,wet)
   elseif a[2]*b[2]<0 then
    local u=-a[2]/(b[2]-a[2]);local water={a[1]+u*(b[1]-a[1]),0,a[3]+u*(b[3]-a[3])}
    world_line(a,water,a[2]<0 and wet or air);world_line(water,b,b[2]<0 and wet or air)
   else world_line(a,b,air) end
  end
 end
 local profile_line=system.millis()
 draw_rod(rod,reel and reel.kind,'idle',t,false,pts)
 local profile_rod=system.millis()
 sim.draw_profile=string.format('pose %d sea %d line %d rod %d',profile_pose-profile_start,profile_sea-profile_pose,profile_line-profile_sea,profile_rod-profile_line)
 Surface.draw()
 if lure and lure.kind=='iso' and sim.line then FloatRig.draw(sim.line,lure,t) end
 if sim.encounter then draw_water_fish(sim.encounter,sim.encounter.position,sim.encounter.previous) end
 if sim.fish and sim.line then
  local f=sim.fish
  draw_water_fish(f,sim.line.p[#sim.line.p],sim.line.prev[#sim.line.prev])
  if sim.state=='landing' then
   local p=project3(sim.line.p[#sim.line.p])
   for i=-1,1 do line(p[1]-12,p[2]+12+i*4,p[1]+12,p[2]+12+i*4,C.foam) end
  end
  return
 end
 if not lure or not sim.line then return end
 local rope=sim.line;local p=rope.p[#rope.p]
 if lure.kind~='iso' and p[3]>CastWorld.near then
  local screen,scale=project3(p)
  local mm=tonumber(lure.length:match('([%d%.]+)%s*MM'))
  local inches=tonumber(lure.length:match('([%d%.]+)%s*IN'))
  local metres=mm and mm/1000 or (inches and inches*.0254 or .06)
  local size=clamp(scale*metres/34*1.35,.055,1.3)
  if not rope.landed or rope.surface or p[2]>.01 then
   local prev=project3(rope.prev[#rope.prev]);local dx,dy=screen[1]-prev[1],screen[2]-prev[2]
   local angle=(dx*dx+dy*dy>.001) and math.atan(dy,dx)+pi or 0
   if screen[1]>-35 and screen[1]<403 and screen[2]>-35 and screen[2]<483 then lure_icon(lure.icon,screen[1],screen[2],size,false,angle,lure) end
  else ellipse(screen[1],screen[2],math.max(.6,size*11),math.max(.4,size*2.3),mix(rgb(0x084773),C.sea2,clamp(-p[2]/8,0,.85))) end
 end
 if rope.landed then
  local c=rope.impact;local elapsed=sim.elapsed
  for ring=0,2 do
   local age=elapsed-ring*.13
   if age>=0 and age<1.5 then
    local radius=.04+age*.65;local prev
    local color=mix(C.foam,C.sea1,clamp(age/1.5,0,1))
    for i=0,28 do local a=i*2*pi/28;local q={c[1]+cos(a)*radius,.005,c[3]+sin(a)*radius}
     if prev then world_line(prev,q,color) end;prev=q
    end
   end
  end
  if elapsed<.48 then for i=1,9 do
   local a=i*2.399;local speed=.6+(i%3)*.25
   local q={c[1]+cos(a)*elapsed*.42,math.max(0,speed*elapsed-4.905*elapsed*elapsed),c[3]+sin(a)*elapsed*.42}
   local sp=project3(q);rect(sp[1],sp[2],1,1,C.white)
  end end
 end
end
-- Review-only posed storyboard. This is not the live fight state machine.
local fight_study={
 {angle=-2.52,pull=.06,x=112,y=282,size=.30,mode='approach'},
 {angle=-2.46,pull=.60,x=151,y=283,size=.33,mode='bite'},
 {angle=-1.98,pull=2.8,x=139,y=290,size=.38,mode='hook'},
 {angle=-1.84,pull=4.8,x=65,y=302,size=.46,mode='left'},
 {angle=-2.18,pull=4.5,x=260,y=305,size=.49,mode='right'},
 {angle=-1.97,pull=5.8,x=134,y=258,size=.29,mode='run'},
 {angle=-2.14,pull=1.7,x=183,y=349,size=.82,mode='recover'},
 {angle=-2.06,pull=.12,x=270,y=381,size=.90,mode='land'},
 {angle=-2.35,pull=.03,x=178,y=332,size=.62,mode='slack'},
 {angle=-1.85,pull=8.0,x=83,y=291,size=.40,mode='overload'},
}
local function draw_fight_study(index)
 local f=fight_study[index] or fight_study[1]
 sea(1)
 local model=physics_for(rod)
 for i=1,360 do RodPhysics.step(model,1/120,{handle_angle=f.angle,line_n=f.pull*2.2}) end
 local pts=RodPhysics.points(model)
 -- Lateral control is posed for the storyboard; the live implementation will
 -- rotate the rod about a world-space grip pivot and solve vector tip load.
 local side=f.mode=='left' and 25 or (f.mode=='right' and -27 or 0)
 for i,q in ipairs(pts) do q[1]=q[1]+side*((i-1)/64)^1.5 end
 local tip=pts[#pts]
 local head_right=f.mode=='right'
 local angle=head_right and pi or (f.mode=='run' and -.15 or .12)
 local mouth=f.x+(head_right and 23 or -23)*f.size
 local mouth_y=f.y+(head_right and -2 or 2)*f.size
 if f.mode=='approach' then mouth=159;mouth_y=280;submerged_lure(1,mouth,mouth_y) end
 local slack=f.mode=='slack' and 42 or (f.mode=='approach' and 13 or 1.5)
 local last
 for i=0,48 do
  local u=i/48
  local q={tip[1]+(mouth-tip[1])*u,tip[2]+(mouth_y-tip[2])*u+slack*sin(pi*u)}
  if last then line(last[1],last[2],q[1],q[2],u<.45 and C.cream or rgb(0x6bafc3)) end
  last=q
 end
 local tint=f.mode=='land' and nil or (f.mode=='recover' and rgb(0x236d88) or rgb(0x10557e))
 fish_icon(fish_types[7],f.x,f.y,f.size,1.3,angle,tint)
 -- Surface disturbances convey direction; no arrow, meter or touch target
 -- is drawn inside the fishing viewport.
 if f.mode~='approach' and f.mode~='slack' then
  local wake=f.mode=='left' and 1 or (f.mode=='right' and -1 or 0)
  for i=1,5 do
   local x=f.x+wake*(12+i*5)+(wake==0 and (i%2==0 and -1 or 1)*(8+i*3) or 0)
   local y=f.y+5+(i%3)*3
   line(x,y,x+4+i,y-1,mix(C.foam,C.sea1,.25))
  end
 end
 if f.mode=='land' then fish_icon(fish_types[7],325,416,.9,0,pi/5) end
 draw_rod(rod,reels[reel_index] and reels[reel_index].kind,'idle',1,false,pts)
end
-- Revised review poses: independent arrival examples, taut loaded line,
-- and a full-screen deck landing. Not yet wired to the live state machine.
local function draw_revised_fight(index)
 local fish=fish_types[7]
 if index<=6 then
  sea(1)
  local poses={
   {x=96,y=300,size=.52,heading=pi+.12,tx=175,ty=306,angle=-2.52,pull=.08,tint=0x146796},
   {x=246,y=330,size=.60,heading=.25,tx=179,ty=311,angle=-2.52,pull=.08,tint=0x155e8b},
   {x=198,y=358,size=.50,heading=.78,tx=167,ty=320,angle=-2.52,pull=.08,tint=0x105c90},
   {x=173,y=321,size=.64,heading=.20,angle=-2.12,pull=3.5,tint=0x145b86},
   {x=86,y=333,size=.94,heading=.20,angle=-1.99,pull=5.8,tint=0x12567e},
   {x=278,y=393,size=1.12,heading=pi+.2,angle=-2.15,pull=2.2,tint=0x25718a},
  }
  local p=poses[index];local model=physics_for(rod)
  for i=1,360 do RodPhysics.step(model,1/120,{handle_angle=p.angle,line_n=p.pull*2.2}) end
  local points=RodPhysics.points(model);local tip=points[#points]
  local mx=p.tx or p.x-23*p.size*cos(p.heading)
  local my=p.ty or p.y-23*p.size*sin(p.heading)
  -- No surface wake on submerged approaches. One target fish per encounter.
  fish_icon(fish,p.x,p.y,p.size,1.1,p.heading,rgb(p.tint))
  if index<=3 then submerged_lure(1,mx,my) end
  -- Split at a single entry point only to tint the underwater portion;
  -- both segments are collinear. No decorative rope bow while hooked.
  local split=.58
  line(tip[1],tip[2],tip[1]+(mx-tip[1])*split,tip[2]+(my-tip[2])*split,C.cream)
  line(tip[1]+(mx-tip[1])*split,tip[2]+(my-tip[2])*split,mx,my,rgb(0x9bc8d7))
  if index==6 then
   for i=1,4 do line(p.x-23+i*8,p.y+13+i%2*3,p.x-18+i*8,p.y+13+i%2*3,C.foam) end
  end
  draw_rod(rod,reels[reel_index] and reels[reel_index].kind,'idle',1,false,points)
  return
 end
 deck_was_visible=false;draw_deck_result(fish,({.05,.36,.725,.97,1.325,1.8})[index-6] or 1.8)
end

local demo_cycle=-1
local function physics_demo(t,now)
 local cycle_id=floor(t/10)
 if cycle_id~=demo_cycle then start_cast();demo_cycle=cycle_id end
end
local function test_physics()
 local baseline=RodPhysics.points(RodPhysics.new(2.03,'M','RF'))
 assert(math.abs(baseline[65][1]-236)<.01 and math.abs(baseline[65][2]-123)<.01)
 local function settle(length,power,action,fish,hz)
  local p=RodPhysics.new(length,power,action)
  for i=1,hz*5 do RodPhysics.step(p,1/hz,{fish_kg=fish}) end
  return p
 end
 local light=settle(2,'M','F',.5,60)
 local heavy=settle(2,'M','F',5,60)
 assert(math.abs(heavy.q)>math.abs(light.q))
 assert(math.abs(settle(2,'L','F',2,60).q)>math.abs(settle(2,'H','F',2,60).q))
 assert(math.abs(settle(2.8,'M','F',2,60).q)>math.abs(settle(1.9,'M','F',2,60).q))
 local regular=RodPhysics.points(settle(2,'M','R',2,60))
 local fast=RodPhysics.points(settle(2,'M','XF',2,60))
 assert(regular[40][2]>fast[40][2])
 local low=settle(2,'M','F',2,30);local high=settle(2,'M','F',2,120)
 assert(math.abs(low.q-high.q)<.002)
 local p=RodPhysics.new(2,'M','RF')
 for i=1,60 do RodPhysics.step(p,1/120,{handle_angle=-3.2,lure_g=14}) end
 assert(math.abs(p.q)>.001)
 for i=1,1200 do RodPhysics.step(p,1/120,{handle_angle=-1.98}) end
 assert(math.abs(p.q)<.002 and math.abs(p.v)<.01)
 for _,r in ipairs(rods) do
  local model=physics_for(r)
  for i=1,1200 do
   RodPhysics.step(model,1/120,{handle_angle=-1.98+.6*sin(i*.1),lure_g=100,fish_kg=20})
   assert(model.q==model.q and math.abs(model.q)<=1.6)
  end
  assert(RodPhysics.points(model)[1][1]==367)
 end
 print('FISHING_PHYSICS_CHECK PASS fish mass, power, length, damping, 30/120Hz, all rods')
end
local function test_model()
 local lengths={}
 for _,r in ipairs(rods) do
  local key=rod_length(r)
  assert(not lengths[key], 'duplicate rod length: '..key)
  lengths[key]=true
 end
 assert(rod_length(rods[1])=="6'8\"" and rod_length(rods[4])=="17'5\"")
 assert(rod_mark(rods[1])=='C' and rod_mark(rods[2])=='S' and rod_mark(rods[6])=='F')
 for _,r in ipairs(rods) do
  for _,reel in ipairs(reels) do
   local allowed=compatible(r,reel,true)
   if r.mount=='spin' then assert(allowed==(reel.kind=='spin')) end
   if r.mount=='bait' then assert(allowed==(reel.kind=='bait' or reel.kind=='round')) end
   if r.kind=='fly' and reel.kind~='fly' then assert(not allowed) end
  end
  if r.kind=='lure' or r.water=='fresh' then assert(not compatible(r,lures[7],false)) end
  if r.kind=='fly' then
   for _,lure in ipairs(lures) do assert(compatible(r,lure,false)==(lure.kind=='fly')) end
  end
 end
 assert(not compatible({kind='iso',mount='spin',water='fresh'},lures[7],false))
 for _,r in ipairs(rods) do
  for _,item in ipairs(lures) do assert(compatible(r,item,false)==(r.kind==item.kind)) end
 end
 assert(compatible(rods[1],reels[1],true) and compatible(rods[1],reels[10],true))
 assert(not compatible(rods[1],reels[3],true) and compatible(rods[2],reels[3],true))
 assert(not compatible(rods[1],reels[5],true))
 assert(not compatible(rods[4],reels[1],true) and compatible(rods[4],reels[3],true))
 assert(compatible(rods[6],reels[6],true) and not compatible(rods[6],reels[3],true))
 assert(not compatible(rods[6],reels[5],true))
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
 print('FISHING_CHECK PASS compatibility / bend / corner anchor / '..(#rods*#powers*#actions*#cycle)..' rod poses')
end
local function on_click(x,y,dx,dy,held)
 if menu then
  if math.abs(dx)>60 and math.abs(dx)>math.abs(dy)*1.3 then
   if dx>0 and tab==1 then menu=false;scene='idle';slide=nil
   else switch_tab(tab+(dx<0 and 1 or -1),dx) end
   return
  end
  if math.abs(dy)>12 and math.abs(dy)>math.abs(dx)*1.3 then
   if y-dy>=62 and y-dy<350 then scrolls[tab]=clamp(scrolls[tab]-dy,0,max_scroll(tab)) end
   return
  end
  if math.abs(dx)>12 or math.abs(dy)>12 then return end
  if y>=16 and y<52 then return end -- Tab headers identify pages; only horizontal drags switch.
  if y>=62 and y<350 and x>=32 and x<344 then
   local local_y=y-66+scrolls[tab]
   if local_y<0 then return end
   local col=floor((x-32)/112);local row=floor(local_y/96)
   if (x-32)%112>=88 or local_y%96>=88 then return end
   local i=row*3+col+1;local list=catalog(tab)
   if not list[i] then return end
   focus[tab]=i
   if tab==1 then rod_index=i;rod=rods[i];reconcile()
   elseif tab~=4 and compatible(rod,list[i],tab==2) then if tab==2 then reel_index=i else lure_index=i end end
   return
  end
  -- The detail panel is a fixed four-field summary.
 else
  if dx < -60 and math.abs(dx)>math.abs(dy)*1.3 then menu=true;tab=1;return end
  if math.abs(dx)>20 or math.abs(dy)>20 then return end
  if not reel_index or not lure_index then menu=true;tab=not reel_index and 2 or 3;return end
  scene='idle'
  if sim.state=='lifting' then return
  elseif sim.state=='landed' then
   if sim.landing_clock>=1.8 then reset_sim();sim.state='retrieved' end
  elseif sim.fish then fight_click(x)
  elseif sim.state=='waiting' then if not start_hook() then retrieve_click() end
  elseif sim.state=='ready' or sim.state=='retrieved' then start_cast() end
 end
end
local function begin_drag(x,y,now)
 if slide or sim.state=='lifting' or (sim.state=='landed' and sim.landing_clock<1.8) then return end
 press={x=x,y=y,t=now,tab=tab,menu=menu,scroll=scrolls[tab],dx=0,dy=0,
  previous_y=y,previous_time=now,velocity=0,reversals=0,direction=0,last_motion=now,turn_y=y}
 if not menu and sim.rod~=rod then reset_sim() end
end
local function update_drag(x,y,now)
 if not press then return end
 now=now or system.millis()
 local dx,dy=x-press.x,y-press.y
 press.dx,press.dy=dx,dy
 if not press.axis then
  if math.abs(dx)>12 and math.abs(dx)>math.abs(dy)*1.3 then press.axis='x'
  elseif math.abs(dy)>12 and math.abs(dy)>math.abs(dx)*1.3 then press.axis='y' end
 end
 if not press.menu and press.axis=='y' and (sim.state=='ready' or sim.state=='retrieved') then
  sim.state='aiming';sim.aim_angle=clamp(-1.98+dy*.0025,-2.50,-1.80)
  local delta=y-press.previous_y
  local elapsed=now-press.previous_time
  if math.abs(delta)>=2 and elapsed>0 then
   local direction=delta>0 and 1 or -1
   if press.direction==0 then press.direction=direction;press.turn_y=y
   elseif direction~=press.direction and math.abs(y-press.turn_y)>30 then
    press.reversals=press.reversals+1;press.direction=direction;press.turn_y=y
   elseif direction==press.direction then press.turn_y=y end
   press.velocity=.4*press.velocity+.6*delta*1000/elapsed
   press.last_motion=now;press.previous_y=y;press.previous_time=now
  end
 end
 if press.menu and press.axis=='y' and press.y>=62 and press.y<350 then
  scrolls[press.tab]=clamp(press.scroll-dy,0,max_scroll(press.tab))
 end
end
local function end_drag(x,y,now)
 if not press then return end
 update_drag(x,y,now)
 local p=press;press=nil
 if not p.menu and p.axis=='y' and sim.state=='aiming' then release_cast(p,now);return end
 if not p.menu and (sim.state=='landed' or sim.state=='lifting') and p.axis then return end
 if p.menu and p.axis=='y' then return end
 if p.menu and p.axis=='x' then
  if p.dx>60 and tab==1 then menu=false;scene='idle';slide=nil;return end
  local target=tab
  if math.abs(p.dx)>60 then target=clamp(tab+(p.dx<0 and 1 or -1),1,4) end
  switch_tab(target,clamp(p.dx,-368,368));return
 end
 on_click(x,y,p.dx,p.dy,now-p.t)
end
local function run_checks()
 do
  local morning=Weather.sample(0,9,'sunny')
  local afternoon=Weather.sample(0,16,'sunny')
  local sunset=Weather.sample(0,18,'sunny')
  local rain=Weather.sample(0,9,'rain')
  assert(not morning.sun_visible and afternoon.sun_visible)
  assert(afternoon.sun_x>260 and afternoon.sun_x-19<368 and sunset.sun_y==203)
  assert(rain.rain and rain.wind==morning.wind and rain.temperature<morning.temperature)
  local before=Weather.sample(359.99,9,'auto');local after=Weather.sample(360,9,'auto')
  assert(math.abs(before.temperature-after.temperature)<=1 and after.rain_amount==0)
  assert(Weather.sample(372.5,9,'auto').rain_amount==.5 and Weather.sample(385,9,'auto').rain_amount==1)
  assert(Weather.sample(0,14,'sunny').temperature>Weather.sample(0,2,'sunny').temperature)
  for _,seed in ipairs({1,719,23891,1234567}) do
   for day=0,7 do
    local low,high=10,-1
    for minute=0,1440,10 do
     local w=Weather.wind_at(day+minute/1440,seed)
     low=math.min(low,w);high=math.max(high,w)
     assert(w>=0 and w<=5.5)
    end
    assert(high-low<=.701,'daily wind variation is bounded')
    assert(math.abs(Weather.wind_at(day+1-.000001,seed)-Weather.wind_at(day+1,seed))<.00001,'continuous midnight wind')
   end
  end
  assert(Weather.wind_at(.5,719)~=Weather.wind_at(.5,23891))
  assert(Weather.sample(1800,9,'sunny').minutes==morning.minutes)
  assert(Weather.sample(0,22,'sunny').night)
  assert(Weather.wind_level(0)==0 and Weather.wind_level(2)==1 and Weather.wind_level(3)==2 and Weather.wind_level(5)==3)
  assert(Weather.feeding_factor(28,Weather.sample(0,22,'sunny'))>Weather.feeding_factor(28,Weather.sample(0,12,'sunny')))
  assert(Weather.feeding_factor(4,Weather.sample(0,12,'sunny'))>Weather.feeding_factor(4,Weather.sample(0,22,'sunny')))
  for i=1,#fish_types do
   for hour=0,23 do
    for _,wind in ipairs({0,2,3,5}) do
     local factor=Weather.feeding_factor(i,{hour=hour,wind=wind})
     assert(factor>0 and factor<2)
    end
   end
  end
  print('FISHING_WEATHER_CHECK PASS')
 end
 test_model()
 test_physics()
 local saved={rod_index,reel_index,lure_index,rod,menu,scene,tab,focus,cast_start,scrolls,detail_page}
 focus={1,1,1,1};scrolls={0,0,0,0};rod_index=1;reel_index=1;lure_index=1;rod=rods[1]
 menu=false
 on_click(70,220,-200,8,250);assert(menu and tab==1)
 on_click(70,220,-200,8,250);assert(menu and tab==2)
 slide=nil;on_click(70,220,-200,8,250);assert(menu and tab==3)
 slide=nil;on_click(70,220,-200,8,250);assert(menu and tab==4)
 slide=nil;on_click(280,220,210,8,250);assert(menu and tab==3)
 slide=nil;on_click(280,220,210,8,250);assert(menu and tab==2)
 slide=nil;on_click(280,220,210,8,250);assert(menu and tab==1)
 slide=nil;on_click(280,220,210,8,250);assert(not menu)
 menu=true;tab=1
 for _,x in ipairs({50,130,220,310}) do on_click(x,32,0,0,80);assert(tab==1 and not slide,'tab labels must not switch on tap') end
 begin_drag(270,32,0);update_drag(110,35,80);end_drag(110,35,160)
 assert(tab==2 and slide and slide.from==1,'header drag must switch tabs');slide=nil;tab=1
 on_click(70,194,0,0,100);assert(rod.kind=='iso' and reel_index==nil and lure_index==nil)
 tab=2;on_click(70,90,0,0,100);assert(reel_index==nil)
 on_click(270,90,0,0,100);assert(reel_index==3)
 tab=3;on_click(70,300,0,0,100);assert(lure_index==7)
 tab=1;on_click(290,194,0,0,100);assert(rod.kind=='fly' and reel_index==nil and lure_index==nil)
 tab=2;on_click(175,194,0,0,100);assert(reel_index==nil)
 on_click(290,194,0,0,100);assert(reel_index==6)
 -- Continuous scrolling follows the finger, clamps at both ends, never equips on release.
 tab=1;local old_focus=focus[1];begin_drag(100,250,0);update_drag(100,113)
 assert(scrolls[1]==math.min(137,max_scroll(1)));end_drag(100,113,200);assert(focus[1]==old_focus)
 begin_drag(100,250,0);end_drag(100,-1000,200);assert(scrolls[1]==max_scroll(1))
 begin_drag(100,100,0);end_drag(100,1500,200);assert(scrolls[1]==0)
 begin_drag(200,200,0);update_drag(170,200);end_drag(170,200,100)
 assert(tab==1 and slide and slide.to==1);slide=nil
 begin_drag(250,200,0);update_drag(100,204);end_drag(100,204,100)
 assert(tab==2 and slide and slide.from==1 and slide.to==2);slide=nil
 local previous=scrolls[2];on_click(358,200,0,0,100);assert(scrolls[2]==previous)
 -- Every item is reachable, including a partially scrolled row and the final row.
 for tt,list in ipairs({rods,reels,lures}) do
  tab=tt
  for i=1,#list do
   scrolls[tt]=clamp(floor((i-1)/3)*96-37,0,max_scroll(tt))
   local y=66+floor((i-1)/3)*96-scrolls[tt]+40
   on_click(76+((i-1)%3)*112,y,0,0,100)
   assert(focus[tt]==i)
  end
 end
 local previous_preview=bag_preview;bag_preview=true;tab=4
 local outfit={rod_index,reel_index,lure_index}
 for i=1,#fish_types do
  scrolls[4]=clamp(floor((i-1)/3)*96-37,0,max_scroll(4))
  on_click(76+((i-1)%3)*112,106+floor((i-1)/3)*96-scrolls[4],0,0,80)
  assert(focus[4]==i)
 end
 assert(rod_index==outfit[1] and reel_index==outfit[2] and lure_index==outfit[3])
 assert(#fish_types==30)
 for _,f in ipairs(fish_types) do assert(f.kg>=f.reference_min_kg and f.kg<=f.reference_max_kg) end
 bag_preview=false;assert(#catalog(4)==0 and max_scroll(4)==0)
 bag_preview=previous_preview
 for _,f in ipairs(fish_types) do
  local b=f.behavior
  local small=fish_behavior_weights(f,.01,1,true,true,0)
  assert(small.run==0,'small fish must not sustain a run')
  local large=fish_behavior_weights(f,math.max(20,b.sprint_min_kg),1,true,true,0)
  assert((large.run>0)==(b.sprint_min_kg>0))
  local gated=fish_behavior_weights(f,50,.2,false,false,0)
  assert(gated.run==0 and gated.jump==0)
  assert(fish_behavior_weights(f,50,1,false,true,1).run==0)
  local sum=0;for _,v in pairs(large) do assert(v>=0 and v<=1);sum=sum+v end
  assert(math.abs(sum-1)<1e-9)
 end
 print('FISHING_BEHAVIOR_CHECK PASS 30 profiles, size/energy/cooldown gates, normalized probabilities')
 print('FISHING_BAG_CHECK PASS 30 specimens, fourth tab, read-only browsing, empty real bag')
 assert(#rods==11 and #reels==19 and #lures==18)
 for _,list in ipairs({rods,reels,lures}) do for _,v in ipairs(list) do assert(brands[v.brand] and v.name and v.model) end end
 rod_index,reel_index,lure_index,rod,menu,scene,tab,focus,cast_start,scrolls,detail_page=table.unpack(saved,1,11)
 press=nil;slide=nil
 local old={rod_index,reel_index,lure_index,rod,menu,scene,tab}
 rod_index=1;rod=rods[1];reel_index=1;lure_index=1;menu=false;scene='idle';reset_sim()
 local function run_cast(hz)
  local max_bend,min_y,max_y=0,1e9,-1e9
  for i=1,hz*12 do
   tick_sim(1/hz)
   max_bend=math.max(max_bend,math.abs(sim.physics.q))
   if sim.line then for _,p in ipairs(sim.line.p) do
    for _,v in ipairs(p) do assert(v==v and math.abs(v)<10000,'nonfinite line') end
   end end
   if sim.state=='waiting' then return sim.range,max_bend end
  end
  error('cast did not land: '..sim.state)
 end
 begin_drag(100,250,0);end_drag(100,250,80);assert(sim.state=='casting')
 local range,bend=run_cast(60)
 do
  local points={};for i,p in ipairs(sim.line.p) do points[i]=copy3(p) end
  local advance=Physics.advance_rope;Physics.advance_rope=nil
  local integrate,damp=Physics.integrate_rope,Physics.damp_rope;Physics.integrate_rope=nil;Physics.damp_rope=nil
  local solver,pose,step=Physics.solve_rope,Physics.rod_pose,Physics.rod_step;Physics.solve_rope=nil;Physics.rod_pose=nil;Physics.rod_step=nil
  start_cast();local reference=run_cast(60);Physics.solve_rope=solver;Physics.rod_pose=pose;Physics.rod_step=step;Physics.integrate_rope=integrate;Physics.damp_rope=damp;Physics.advance_rope=advance
  assert(math.abs(reference-range)<1e-6 and #points==#sim.line.p,'native/reference cast range')
  for i,p in ipairs(points) do for axis=1,3 do
   assert(math.abs(p[axis]-sim.line.p[i][axis])<1e-6,'native/reference rope position')
  end end
  print('FISHING_NATIVE_PHYSICS_CHECK PASS Lua/native XPBD trajectory and node positions')




 end
 assert(range>5 and bend>.01,'cast must move forward and flex')
 assert(sim.line.impact[2]==0 and sim.line.paid>=.65)
 local _,screen=world_rod(sim.physics)
 for i=1,120 do tick_sim(1/120) end
 local _,lowered=world_rod(sim.physics);assert(lowered[65][2]>175,'waiting rod too high')
 on_click(100,250,0,0,80);assert(sim.state=='waiting' and sim.retrieve.queue==1)
 start_cast()
 local r30=run_cast(30)
 start_cast();local r120=run_cast(120)
 assert(math.abs(r30-r120)<.03,'frame-rate dependent cast')
 local near,ns=project3({0,1,2});local far,fs=project3({0,1,20})
 assert(math.abs(ns/fs-10)<.001 and far[2]<near[2])
 reel_index=10;tick_sim(.01);assert(sim.state=='ready' and sim.reel_index==10)
 lure_index=nil;tick_sim(.01);assert(sim.lure_index==nil)
 rod_index=6;rod=rods[6];reel_index=6;lure_index=8;reset_sim()
 release_cast({velocity=-1800,last_motion=100,reversals=0},100);assert(sim.state=='ready')
 start_cast();local flyrange=run_cast(60)
 for ri,r in ipairs(rods) do
  rod_index=ri;rod=r;reel_index=nil;lure_index=nil
  for i,item in ipairs(reels) do if compatible(rod,item,true) then reel_index=i;break end end
  for i,item in ipairs(lures) do if compatible(rod,item,false) then lure_index=i;break end end
  assert(reel_index and lure_index)
  start_cast();local distance=run_cast(60)
  assert(sim.line.impact[3]>1.3,'cast went behind boat: '..r.model)
  print(string.format('FISHING_RIG_CHECK %s range=%.2f depth=%.2f',r.model,distance,sim.line.impact[3]))
 end
 local function free_sink(item,seconds)
  local profile=water_profile(item);local y,v=-.1,0
  for i=1,seconds*240 do local x; x,v=wet_velocity(profile,0,v,0,1/240);y=y+v/240 end
  return y
 end
 local light={};for k,v in pairs(lures[14]) do light[k]=v end;light.weight=13
 assert(free_sink(lures[14],10)<free_sink(light,10),'heavier same-shape sinking bait should fall faster')
 assert(water_profile(lures[1]).kind=='float' and water_profile(lures[5]).kind=='surface')
 assert(water_profile(lures[11]).kind=='suspend')
 local function slack(line)
  local total=0
  for i=1,#line.p-1 do local a,b=line.p[i],line.p[i+1];total=total+length3(a[1]-b[1],a[2]-b[2],a[3]-b[3]) end
  local a,b=line.p[1],line.p[#line.p]
  return total-length3(a[1]-b[1],a[2]-b[2],a[3]-b[3])
 end
 rod_index=1;rod=rods[1];reel_index=1;lure_index=14;start_cast();run_cast(60)
 local paid=sim.line.paid
 for i=1,120 do tick_sim(1/60) end
 local depth2=-sim.line.p[#sim.line.p][2];local slack2=slack(sim.line)
 for i=1,60*38 do tick_sim(1/60) end
 local depth40=-sim.line.p[#sim.line.p][2];local slack40=slack(sim.line)
 print(string.format('FISHING_SINK_METRICS depth %.3f -> %.3f slack %.3f -> %.3f',depth2,depth40,slack2,slack40))
 assert(depth40>depth2+.2 and slack40<slack2,'sinking must gradually remove slack')
 assert(math.abs(sim.line.paid-paid)<1e-8,'waiting must not pay out extra line')
 lure_index=5;start_cast();run_cast(60)
 for i=1,600 do tick_sim(1/60) end
 assert(math.abs(sim.line.p[#sim.line.p][2])<1e-8,'topwater must remain afloat')
 local function retrieve_case(ri,li,hz)
  rod_index=ri;rod=rods[ri];lure_index=li
  for i,item in ipairs(reels) do if compatible(rod,item,true) then reel_index=i;break end end
  start_cast();run_cast(hz)
  local initial=sim.line.paid;local initial_nodes=#sim.line.p
  on_click(100,250,0,0,80)
  local q0=sim.physics.q;local bend=0
  for i=1,hz do tick_sim(1/hz);bend=math.max(bend,math.abs(sim.physics.q-q0)) end
  local first=initial-sim.line.paid
  assert(first>0 and first<1 and bend>1e-5,'click must wind one stroke and move rod')
  if ri==4 then
   assert(first<.13 and not sim.retrieve.full,'single float click must be gentle')
   for i=1,4 do on_click(100,250,0,0,80);tick_sim(.1) end
   assert(sim.retrieve.full,'float rapid clicks should enable full retrieve')
  end
  for i=1,hz*45 do
   if ri~=4 and i%math.max(1,math.floor(hz*.7))==0 then on_click(100,250,0,0,80) end
   tick_sim(1/hz)
   if sim.state=='retrieved' then break end
  end
  assert(sim.state=='retrieved','retrieve did not finish')
  local paid=sim.line.paid
  assert(math.abs(paid-sim.line.minimum)<1e-6 and #sim.line.p<initial_nodes)
  for i=1,hz*3 do tick_sim(1/hz) end
  assert(sim.state=='retrieved' and sim.line.paid==paid,'completed retrieval must retain leader')
  local sum=0;for _,v in ipairs(sim.line.rest) do assert(v>0);sum=sum+v end
  assert(math.abs(sum-paid)<1e-6,'rope material length must be conserved')
  local a,b=sim.line.p[1],sim.line.p[#sim.line.p]
  local gap=length3(a[1]-b[1],a[2]-b[2],a[3]-b[3])
  assert(gap>paid*.8 and gap<paid+.08,'keep hanging leader without overstretch')
  for _,p in ipairs(sim.line.p) do for _,v in ipairs(p) do assert(v==v and math.abs(v)<1000) end end
  print(string.format('FISHING_RETRIEVE_CHECK %s %dHz first=%.3f gap=%.3f bend=%.4f',lures[li].model,hz,first,gap,bend))
  on_click(100,250,0,0,80);assert(sim.state=='casting','next click must recast')
  return first,gap
 end
 local first30,gap30=retrieve_case(1,1,30)
 local first120,gap120=retrieve_case(1,1,120)
 assert(math.abs(first30-first120)<1e-6 and math.abs(gap30-gap120)<.02)
 retrieve_case(1,3,60);retrieve_case(6,8,60);retrieve_case(4,7,60);retrieve_case(1,14,60)
 -- Actual hook/fight/catch path, with deterministic inputs and independent records.
 local bag_before=#catches
 local function fight_fixture(index,kg)
  rod_index=1;rod=rods[1];reel_index=1;lure_index=1;menu=false;scene='idle'
  start_cast();run_cast(60)
  local species=fish_types[index]
  sim.encounter={species=species,index=index,kg=kg,cm=math.floor(species.cm*(kg/species.kg)^(1/3)),phase='take',clock=.45}
  on_click(180,260,0,0,80)
  assert(sim.state=='fight' and sim.fish and sim.line.mass==kg)
  return sim.fish
 end
 assert(habitat_weight(29,10,0,lures[1])==0,'offshore tuna must not spawn in the nearshore pool')
 assert(habitat_weight(5,10,1,lures[1])==0 and habitat_weight(5,10,1,lures[7])>0,'grazer diet filtering')
 assert(habitat_weight(20,12,0,lures[14])==0 and habitat_weight(20,12,2,lures[14])>0,'grouper water layer')
 random_seed=142857
 local f=fight_fixture(7,1.2)
 on_click(20,240,0,0,80);assert(f.side<0)
 for i=1,30 do tick_sim(1/60) end
 assert(sim.physics.yaw<0,'left input rotates world-space rod left')
 on_click(340,240,0,0,80);on_click(340,240,0,0,80);assert(f.side>0)
 for i=1,6000 do
  if sim.state=='landing' then on_click(184,240,0,0,80)
  elseif sim.fish and i%36==0 and sim.fish.action~='run' then on_click(184,240,0,0,80) end
  tick_sim(1/60)
  if #catches>bag_before then break end
 end

 assert(#catches==bag_before+1,'controlled reeling should land the fish')
 local record=catches[#catches]
 assert(record~=fish_types[7] and record.kg==1.2 and fish_value(record)==46)
 complete_catch();assert(#catches==bag_before+1,'catch must only be recorded once')
 print('FISHING_LIVE_CATCH_CHECK PASS bite input, lateral control, reeling, boat-side catch, unique bag record')
 assert(sim.state=='landed' and sim.landing_clock<1.8,'auto landing starts deck sequence')
 on_click(184,220,0,0,80);assert(sim.state=='landed','early click cannot skip two flops')
 for i=1,120 do tick_sim(1/60) end
 assert(sim.state=='landed' and sim.landing_clock==1.8,'result remains on screen')
 on_click(184,220,0,0,80);assert(sim.state=='retrieved' and #catches==bag_before+1)
 on_click(184,220,0,0,80);assert(sim.state=='casting','fresh next click casts')
 assert(deck_pose(.35).height==0 and deck_pose(.95).height==0 and deck_pose(1.5).height==0)
 assert(deck_pose(.725).height>deck_pose(1.325).height)
 -- Randomized underwater origins cover both sides and a real depth range.
 local fixture={p={{0,-.4,15}}};local left,right,shallow,deep=false,false,1e9,-1e9
 local arrivals=0;random_seed=771
 for i=1,120 do
  local e=pick_encounter(fixture,lures[1])
  if e then
  arrivals=arrivals+1;assert(e.position[2]<0)
  left=left or e.position[1]<0;right=right or e.position[1]>0
  shallow=math.min(shallow,-e.position[2]);deep=math.max(deep,-e.position[2])
  assert(e.position[2]>=-bottom_depth(e.position[1],e.position[3]))
 end
 end
 assert(arrivals>0 and arrivals<120,'encounter opportunities include both feeding and no bite')
 assert(left and right and deep-shallow>.5,'arrival directions and water depths must vary')
 print('FISHING_DECK_CHECK PASS auto landing, exactly once, two hops, modal result, next cast, underwater origins')

 random_seed=77
 f=fight_fixture(11,6);sim.line_setup.drag=2;f.action='run';f.action_duration=6;f.cooldown=8
 local paid_start=sim.line.paid
 for i=1,360 do tick_sim(1/60) end
 assert(sim.fish and sim.line.paid>paid_start+.05,'big fish must pull line through the drag')
 print(string.format('FISHING_DRAG_CHECK PASS paid %.3f -> %.3f',paid_start,sim.line.paid))
 f=fight_fixture(1,.2)
 for i=1,30 do choose_fish_action(f,sim.line.p[#sim.line.p]);assert(f.action~='run') end
 print('FISHING_SMALL_FISH_CHECK PASS no sustained run')
 f=fight_fixture(7,1)
 sim.line.paid=sim.line.paid+4;sim.line.rest[1]=sim.line.rest[1]+4
 f.action='shake';f.action_duration=20
 for i=1,600 do tick_sim(1/120);if not sim.fish then break end end
 assert(not sim.fish and sim.last_loss=='SLACK LINE','slack plus headshake should unhook')
 f=fight_fixture(7,1)
 sim.line_setup.strength=.5
 for i=1,360 do tick_sim(1/120);if not sim.fish then break end end
 assert(not sim.fish and sim.last_loss=='LINE BREAK','overloaded weak line must break')
 print('FISHING_LOSS_CHECK PASS slack and physical overloading')
 local function fight_at_rate(hz)
  local f=fight_fixture(7,1);random_seed=91
  for i=1,hz*4 do
   if i%hz==0 then fight_click(184) end
   tick_sim(1/hz)
  end
  assert(sim.fish)
  local p=sim.line.p[#sim.line.p]
  return {p[1],p[2],p[3],sim.line.paid}
 end
 local low,high=fight_at_rate(30),fight_at_rate(120)
 for i=1,4 do assert(math.abs(low[i]-high[i])<.04,'fight depends on render rate') end
 print('FISHING_LIVE_RATE_CHECK PASS 30/120Hz')
 do
  rod_index=1;rod=rods[1];reel_index=1;lure_index=5;start_cast();run_cast(60)
  Surface.effects={};retrieve_click()
  for i=1,40 do tick_sim(1/60) end
  assert(#Surface.effects==1 and Surface.effects[1].kind=='pop','one pop per stroke')
  retrieve_click();for i=1,40 do tick_sim(1/60) end
  assert(#Surface.effects==2,'second pop stroke makes one new splash')
  lure_index=6;start_cast();run_cast(60)
  local directions={}
  for stroke=1,2 do
   local p=sim.line.p[#sim.line.p];local x,z=p[1],p[3]
   local dx,dz=sim.line.p[1][1]-x,sim.line.p[1][3]-z
   retrieve_click();for i=1,30 do tick_sim(1/60) end
   p=sim.line.p[#sim.line.p]
   directions[stroke]=(p[1]-x)*dz-(p[3]-z)*dx
   assert(math.abs(p[2])<.001,'pencil stays at the surface')
  end
  assert(directions[1]*directions[2]<0,'pencil must reverse its actual lateral travel')
  lure_index=5;start_cast();run_cast(60);Surface.effects={}
  local p=sim.line.p[#sim.line.p]
  sim.encounter={species=fish_types[7],index=7,kg=1.2,cm=48,phase='peck',clock=.60,
   position={p[1],-.2,p[3]},rise_y=-.2,surface_attack=true,bite_side=1}
  fish_enabled=true
  for i=1,30 do tick_sim(1/60) end
  fish_enabled=false
  assert(#Surface.effects==1 and Surface.effects[1].kind=='attack')
  assert(sim.encounter.phase=='take' and sim.line.surface==false and sim.line.p[#sim.line.p][2]<-.02,'surface strike pulls the rig underwater')
  assert(start_hook() and sim.fish.action=='dive','surface hook transitions into a dive')
  local depth=sim.line.p[#sim.line.p][2]
  for i=1,30 do tick_sim(1/60) end
  assert(sim.fish and sim.line.p[#sim.line.p][2]<depth,'hooked fish continues down')
  print('FISHING_SURFACE_CHECK PASS alternating pencil trajectory, one pop per stroke, surface attack, dive transition')
  f=fight_fixture(7,1.2);f.nearboat=true;f.action='recover'
  local tip=sim.line.p[1]
  sim.line.p[2]={1.05,-.06,2.8};sim.line.prev[2]=copy3(sim.line.p[2]);sim.line.tension=1
  sim.line.paid=length3(1.05-tip[1],-.06-tip[2],2.8-tip[3]);sim.line.rest[1]=sim.line.paid
  step_fish_after(CastWorld.h,tip);assert(sim.state=='fight','visible distant water is not the rail')
  sim.line.p[2]={1.05,-.06,1.85};sim.line.prev[2]=copy3(sim.line.p[2]);sim.line.tension=1
  sim.line.paid=length3(1.05-tip[1],-.06-tip[2],1.85-tip[3]);sim.line.rest[1]=sim.line.paid
  step_fish_after(CastWorld.h,tip);assert(sim.state=='lifting','lift only at visible boat waterline')
  tick_sim(.15);assert(sim.state=='lifting' and sim.line.p[2][2]>0,'only explicit lift raises fish')
  local count=#catches;on_click(184,220,0,0,80);assert(sim.state=='lifting' and #catches==count)
  tick_sim(.31);assert(sim.state=='landed' and #catches==count+1)
  print('FISHING_RAIL_CHECK PASS submerged approach, screen-space rail gate, explicit lift, single catch')

 end

 while #catches>bag_before do catches[#catches]=nil end
 print('FISHING_WATER_CHECK PASS mass/buoyancy/drag, gradual slack removal, fixed paid length, topwater exception')
 print(string.format('FISHING_TRAJECTORY_CHECK PASS range=%.2fm fly=%.2fm 30/120Hz delta=%.6f',range,flyrange,math.abs(r30-r120)))
 rod_index,reel_index,lure_index,rod,menu,scene,tab=table.unpack(old,1,7)
 reset_sim();press=nil;slide=nil
 print('FISHING_CAST_CHECK PASS click stroke, XPBD line, water impact, perspective, lowered wait, gear reset, fly false casts')
 do
  rod_index=4;rod=rods[4];reel_index=3;lure_index=7
  start_cast();run_cast(60)
  local lure=equipped_lure();local rope=sim.line
  local float=FloatRig.position(rope,lure);local hook=FloatRig.hook(rope,lure)
  assert(rope.surface and math.abs(float[2])<.12 and hook[2]<-.5)
  local enabled=fish_enabled;fish_enabled=true
  sim.encounter={species=fish_types[2],index=2,kg=.5,cm=24,phase='peck',clock=0,
   position=copy3(hook),previous=copy3(hook),surface_attack=false,bite_side=1}
  Surface.effects={}
  for i=1,170 do step_encounter(1/240,lure) end
  assert(sim.encounter.phase=='take' and sim.encounter.position[2]<-.5 and #Surface.effects==0)
  assert(start_hook());assert(sim.line.p[#sim.line.p][2]<-.5)
  local f=FloatRig.position(sim.line,lure);local mouth=sim.line.p[#sim.line.p]
  assert(length3(f[1]-mouth[1],f[2]-mouth[2],f[3]-mouth[3])>.6)
  detach_fish('TEST RELEASE');assert(sim.line.surface and not sim.line.fish)
  fish_enabled=enabled
  print('FISHING_FLOAT_CHECK PASS visible float geometry, separate underwater hook, no surface attack, leader retained on strike/release')
 end
 rod_index,reel_index,lure_index,rod,menu,scene,tab=table.unpack(old,1,7)
 reset_sim();press=nil;slide=nil
 print('FISHING_INPUT_CHECK PASS continuous scroll, tab drags, no accidental equip, bounds, all 48 items')
end
if A.check=='1' then run_checks() end


init_fishing_audio()
D.begin_frame({clear=true,color=C.sky})
-- Build the procedural deck command list before accepting input, not on the first catch.
draw_deck_result(fish_types[7],0);deck_was_visible=false;deck_result_visible=false
D.clear(C.sky)
touch.sync()
print('FISHING_READY 368x448 Lua-only geometry; swipe left opens gear; swipe right returns to sea')
reset_sim()
if scene=='deck-demo' or scene=='deck-record' then
 sim.state='landed';sim.landing_clock=fixed and math.min(1.8,fixed/1000) or 0
 local example={};for k,v in pairs(fish_types[7]) do example[k]=v end
 example.cm=48;example.kg=1.2;sim.landed_record=example
end
local function fight_demo_step(h)
 if sim.encounter and sim.encounter.phase=='take' and sim.encounter.clock>.35 then start_hook() end
 if sim.fish then
  sim.demo_clock=(sim.demo_clock or 0)+h
  if sim.demo_clock>.6 then
   local rope=sim.line;local f=sim.fish
   if sim.state=='landing' then fight_click(184)
   elseif f.action~='run' then fight_click(184) end
   sim.demo_clock=0
  end
 end
 tick_sim(h)
end
local function retrieve_demo_step(h)
 if sim.state=='waiting' then
  sim.demo_clock=(sim.demo_clock or 0)+h
  if sim.demo_clock>=.7 then retrieve_click();sim.demo_clock=0 end
 end
 tick_sim(h)
end
if scene=='retrieve-demo' or scene=='fight-demo' then start_cast() end
if fixed and scene=='fight-demo' then
 for i=1,math.floor(fixed/1000/CastWorld.h+.5) do fight_demo_step(CastWorld.h) end
end
if fixed and scene=='retrieve-demo' then
 for i=1,math.floor(fixed/1000/CastWorld.h+.5) do retrieve_demo_step(CastWorld.h) end
end
if fixed and scene=='cast-demo' then
 start_cast()
 for i=1,math.floor(fixed/1000/CastWorld.h+.5) do tick_sim(CastWorld.h) end
end
local menu_signature
local frames,perf_start=0,system.millis()
local frame_samples={}
local previous_frame=system.millis()
local recording_frame=0
local recording=scene=='cast-record' or scene=='deck-record'
while true do
 local now=system.millis()
 if menu then deck_was_visible=false;deck_result_visible=false else menu_signature=nil end
 frame_draw_calls=0
 for name in pairs(kernel_times) do kernel_times[name]=0 end
 if now>previous_frame then frame_samples[#frame_samples+1]=now-previous_frame end
 local t=recording and recording_frame/60 or (fixed and fixed/1000 or (now-start)/1000)
 if not fixed and not recording then
  local ok,info=pcall(touch.poll)
  if ok then
   if sim.touch_error then print('FISHING_TOUCH recovered');sim.touch_error=nil end
   if info.just_pressed then begin_drag(info.x,info.y,now) end
   if press then update_drag(info.x,info.y,now) end
   if info.just_released then end_drag(info.x,info.y,now) end
  else
   if not sim.touch_error then print('FISHING_TOUCH retry '..tostring(info));sim.touch_error=true end
   press=nil -- Never turn an interrupted gesture into a cast/equipment click.
  end
 end
 if not fixed and not recording and (scene=='physics-demo' or scene=='cast-demo') then physics_demo(t,now) end
 if recording then
  if scene=='deck-record' then sim.landing_clock=math.min(1.8,recording_frame/60)
  elseif recording_frame==0 then start_cast() else tick_sim(1/60) end
  recording_frame=recording_frame+1
 elseif not fixed then
  local dt=clamp((now-previous_frame)/1000,0,.1)
  if scene=='retrieve-demo' then retrieve_demo_step(dt) elseif scene=='fight-demo' then fight_demo_step(dt) else tick_sim(dt) end
 end
 update_fishing_audio(now)
 previous_frame=now
 local draw_begin=system.millis()
 if scene:match('^fight%-revision%-%d+$') then
  draw_revised_fight(tonumber(scene:match('(%d+)$')))
 elseif scene:match('^fight%-study%-%d+$') then
  draw_fight_study(tonumber(scene:match('(%d+)$')))
 elseif scene=='fish-atlas' or scene=='fish-atlas-2' or scene=='fish-atlas-3' then
  local page=scene=='fish-atlas-2' and 2 or (scene=='fish-atlas-3' and 3 or 1)
  D.clear(rgb(0x10374b))
  centered(0,10,368,'WATER STUDY / '..page..' OF 3',C.cream,1.5)
  for slot=1,10 do
   local f=fish_types[(page-1)*10+slot]
   local x=8+(slot-1)%2*180;local y=37+floor((slot-1)/2)*81
   rect(x,y,172,76,rgb(0x17485b))
   fish_icon(f,x+75,y+31,1.85,0)
   centered(x,y+63,172,f.short,C.cream,1)
  end
 elseif scene=='lure-view' then
  D.clear(C.ink)
  local item=lures[tonumber(A.lure) or 1]
  centered(0,24,368,brands[item.brand],C.cyan,2)
  centered(0,62,368,item.name,C.cream,1.5)
  centered(0,92,368,item.model,C.cream,2)
  lure_icon(item.icon,204,210,5,false,0,item)
  centered(0,324,368,item.length..' / '..item.weight..'G',C.cream,1.5)
  centered(0,356,368,string.upper(item.bill or 'short')..' BILL',C.cyan,1.5)
  if item.depth_m then centered(0,385,368,'DIVING DEPTH '..item.depth_m..'M',C.cyan,1) end
 elseif scene=='reel-view' then
  D.clear(C.ink)
  local item=reels[tonumber(A.reel) or 1]
  centered(0,24,368,brands[item.brand],C.cyan,2)
  centered(0,58,368,item.name,C.cream,1.5)
  centered(0,84,368,item.model,C.cream,1)
  stroke(100,120,270,368,6,C.grip)
  reel_icon(item,184,244,8,math.atan(248,170))
  centered(0,396,368,'SIDE / TOP - LUA GEOMETRY',C.cyan,1)
 elseif scene=='cq' then
  D.clear(C.ink)
  centered(0,22,368,'SHIMANO',C.cyan,3)
  centered(0,55,368,'CALCUTTA CONQUEST',C.cream,2)
  inventory_reel('round',165,182,false,C.gold,3)
  centered(0,312,368,'100 RIGHT',C.cream,2)
  centered(0,346,368,'5.6:1  220G  DRAG 4.5KG',C.cream,1.5)
  centered(0,371,368,'NYLON 12LB / 100M',C.cream,1.5)
  centered(0,414,368,'LUA PIXEL GEOMETRY / 3X',C.cyan,1.5)
 elseif menu then
  local signature=table.concat({tab,focus[tab],scrolls[tab],rod_index,reel_index or 0,lure_index or 0,#catches},':')
  if slide or (press and press.axis=='x') or signature~=menu_signature then
  D.clear(C.wood)
  if slide then
   local progress=clamp((now-slide.start)/180,0,1)
   local eased=1-(1-progress)^3
   local direction=slide.to>slide.from and -1 or 1
   local destination=slide.to==slide.from and 0 or direction*368
   local x=slide.x+(destination-slide.x)*eased
   draw_menu(slide.from,x)
   if slide.to~=slide.from then draw_menu(slide.to,x-direction*368) end
   if progress>=1 then slide=nil end
  elseif press and press.menu and press.axis=='x' then
   local x=clamp(press.dx,-368,368)
   local target=tab+(x<0 and 1 or -1)
   if target<1 or target>4 then x=x*.25 end
   draw_menu(tab,x)
   if target>=1 and target<=4 then draw_menu(target,x+(x<0 and 368 or -368)) end
  else draw_menu(tab,0) end
  menu_signature=slide and nil or signature
  end
 elseif scene=='weather-demo' then sea(t);draw_rod(rod,reels[reel_index] and reels[reel_index].kind,'idle',t,false)
 elseif scene=='deck-demo' or scene=='deck-record' then draw_deck_result(sim.landed_record,sim.landing_clock)
 elseif recording or scene=='cast-demo' or scene=='retrieve-demo' or scene=='fight-demo' or (not fixed and (scene=='idle' or scene=='physics-demo')) then draw_live(t)
 else
  local mode,r,reel=scene,rod,reels[reel_index] and reels[reel_index].kind or nil
  if scene=='demo' then
   mode=cycle[floor(t/4)%#cycle+1]
   if mode=='iso' then r=rods[4];reel='spin'
   elseif mode=='fly-back' or mode=='fly-send' then r=rods[6];reel='fly' end
  end
  scene_draw(mode,t,r,reel)
 end
 if not menu and sim.state~='landed' and (scene=='idle' or scene=='weather-demo' or scene=='fight-demo' or scene=='cast-demo' or scene=='retrieve-demo') then Weather.hud() end
 local present_begin=system.millis()
 local pixels,regions=D.present({retained=true})
 local present_end=system.millis()
 collectgarbage('step',8)
 frames=frames+1
 if now-perf_start>=2000 then
  table.sort(frame_samples)
  local p95=frame_samples[math.max(1,math.ceil(#frame_samples*.95))] or 0
  print(string.format('FISHING_PERF %.1f FPS / %s / p95 %dms / draw %d / Lua %.0fKB / tx %dpx %d regions / sim %d draw %d present %dms',frames*1000/(now-perf_start),menu and 'menu' or sim.state,p95,frame_draw_calls,collectgarbage('count'),pixels,regions,draw_begin-now,present_begin-draw_begin,present_end-present_begin))
  if sim.draw_profile then print('FISHING_DRAW '..sim.draw_profile) end
  if A.profile=='amoled' then
   local parts={};for name,value in pairs(kernel_times) do parts[#parts+1]=name..'='..value end
   print('FISHING_KERNEL '..table.concat(parts,' '))
  end
  frames=0;perf_start=now;frame_samples={}
 end
 delay.delay_ms(math.max(1,math.floor(1000/Budget.fps)-(system.millis()-now)))
end
