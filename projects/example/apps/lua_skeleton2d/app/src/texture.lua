local d,vm,sk=require('display'),require('vmath'),require('skeleton2d')
local clock,delay=require('system'),require('delay')
local rgba=string.char(255,0,255,255):rep(4)..
 string.char(255,0,255,255,255,0,0,255,0,255,0,128,255,0,255,255)..
 string.char(255,0,255,255,0,0,255,0,255,255,255,255,255,0,255,255)..
 string.char(255,0,255,255):rep(4)
local tex=d.texture(4,4,rgba)
local cases={{1,0,0,1,1,1},{0,1,-1,0,5,1},{2,0,0,2,1,1},
 {-2,0,0,2,6,1},{1,0,.5,1,2,2},{0,0,0,1,2,2},{1,2,2,4,0,0},
 {.8,.6,-.6,.8,4.125,3.25},{.5,0,0,.5,2,2},{1,0,0,1,-1,-1}}
local def=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},
 parts={{1,1,0,1,0,0,0,1,1}},clips={}}
local actor=def:instance()
local function pixels()
 d.clear('black')
 local cell=0
 for _,m in ipairs(cases) do for anchor=0,1 do for clip=0,1 do
  local resource={{texture=tex,x=1,y=1,width=2,height=2,anchor_x=anchor,anchor_y=anchor*.5}}
  local plain=d.texture_batch(resource,1)
  local writer,batch=sk.textures(def,resource)
  for mode=0,1 do
   local ox,oy=(cell%20)*12,(cell//20)*12
   local root={m[1],m[2],m[3],m[4],m[5]+ox,m[6]+oy}
   if mode==0 then
    local row=vm.buffer(7);row:load({1,table.unpack(root)});d.update_textures(plain,row)
    d.draw_textures(plain,ox+clip,oy+clip,ox+8-clip,oy+8-clip)
   else
    actor:evaluate(root);sk.update_textures(writer,actor)
    d.draw_textures(batch,ox+clip,oy+clip,ox+8-clip,oy+8-clip)
   end
   cell=cell+1
  end
 end end end
 -- Late invalid publication must preserve the prior valid batch.
 local r={{texture=tex,x=1,y=1,width=2,height=2,anchor_x=0,anchor_y=0}}
 local batch=d.texture_batch(r,2);local rows=vm.buffer(14)
 rows:load({1,1,0,0,1,0,60,1,1,0,0,1,4,60});d.update_textures(batch,rows)
 rows:set(8,2);assert(not pcall(d.update_textures,batch,rows))
 d.draw_textures(batch,0,60,8,64)
 assert(not pcall(d.texture,1,1,'bad'))
 assert(not pcall(d.draw_textures,batch,0,0,241,240))
 d.present()
 print('TEXTURE FRAME cases=80 errors=PASS')
end
local function percentile(a,p) table.sort(a);return a[math.ceil(#a*p)] end
local function benchmark()
 local texture=d.texture(32,32,string.char(255,80,16,255,20,200,80,128,0,0,255,0,255,255,255,255):rep(256))
 for _,n in ipairs({1,16}) do
  local parts={}
  for i=1,n do parts[i]={1,1,i,1,((i-1)%4)*28-56,((i-1)//4)*28-56,0,1,1} end
  local definition=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},parts=parts,
   clips={{duration_us=1000000,tracks={{bone=1,channel=3,linear=true,shortest=false,keys={{0,-.3},{1000000,.3}}}}}}}
  local a=definition:instance();local w,b=sk.textures(definition,{{texture=texture,x=0,y=0,width=32,height=32,anchor_x=16,anchor_y=16}})
  local root={1,0,0,1,120,120}
  for _,edge in ipairs({120,240}) do for run=1,3 do
   local stages={{},{},{},{},{},{}};for j=1,6 do for i=1,3000 do stages[j][i]=0 end end
   collectgarbage('collect');local vm_before=collectgarbage('count')
   local submitted_pixels,submitted_rects,over_budget=0,0,0
   for frame=1,3300 do
    local t0=clock.micros();a:sample(1,frame*33333,'repeat');a:evaluate(root)
    local t1=clock.micros();sk.update_textures(w,a);local t2=clock.micros()
    d.fill_rect(0,0,edge,edge,'black');local t3=clock.micros()
    d.draw_textures(b,0,0,edge,edge);local t4=clock.micros();local px,rects=d.present();local t5=clock.micros()
    if frame>300 then local i=frame-300
     stages[1][i]=t1-t0;stages[2][i]=t2-t1;stages[3][i]=t3-t2
     stages[4][i]=t4-t3;stages[5][i]=t5-t4;stages[6][i]=t5-t0
     submitted_pixels=submitted_pixels+px;submitted_rects=submitted_rects+rects
     if t5-t0>33333 then over_budget=over_budget+1 end
    end
   end
   print(string.format('TEXTURE WORKLOAD bones=1 parts=%d texture_bytes=4096 edge=%d run=%d samples=3000 warmup=300 avg_pixels=%.1f avg_rects=%.1f over_33ms=%d',n,edge,run,submitted_pixels/3000,submitted_rects/3000,over_budget))
   local names={'sample_evaluate','adapter','recovery','texture_raster','present','total'}
   local vm_after=collectgarbage('count')
   for j=1,6 do print(string.format('TEXTURE BENCH parts=%d edge=%d run=%d phase=%s p50_us=%d p95_us=%d p99_us=%d max_us=%d vm_before_kib=%.1f vm_after_kib=%.1f',n,edge,run,names[j],percentile(stages[j],.5),percentile(stages[j],.95),percentile(stages[j],.99),stages[j][3000],vm_before,vm_after)) end
   delay.delay_ms(1)
  end end
 end
 print('TEXTURE BENCH COMPLETE MCU=SKIP')
end
benchmark();pixels()
while true do delay.delay_ms(16) end
