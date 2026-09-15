local d,vm,sk=require('display'),require('vmath'),require('skeleton2d')
local probe=require('raster_test')
local function bad(f,...) assert(not pcall(f,...)) end
local rgba=string.char(255,0,255,255):rep(4)..
 string.char(255,0,255,255,255,0,0,255,0,255,0,128,255,0,255,255)..
 string.char(255,0,255,255,0,0,255,0,255,255,255,255,255,0,255,255)..
 string.char(255,0,255,255):rep(4)
local tex
probe.measure('texture_init_4x4',function() tex=d.texture(4,4,rgba) end,1,true)
local attachments={{texture=tex,x=1,y=1,width=2,height=2,anchor_x=0,anchor_y=0}}
local batch=d.texture_batch(attachments,2)
local row=vm.buffer(7)
local def=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},
 parts={{1,1,0,1,0,0,0,1,1}},clips={{duration_us=1000000,tracks={
 {bone=1,channel=3,linear=true,shortest=false,keys={{0,0},{1000000,1}}}}}}}
local actor=def:instance()
local writer,sprites=sk.textures(def,attachments)
attachments[1].x=400 -- copied metadata
tex,attachments,rgba=nil,nil,nil
collectgarbage('collect')
local cases={{1,0,0,1,1,1},{0,1,-1,0,5,1},{2,0,0,2,1,1},
 {-2,0,0,2,6,1},{1,0,.5,1,2,2},{0,0,0,1,2,2},{1,2,2,4,0,0},
 {.8,.6,-.6,.8,4.125,3.25},{.5,0,0,.5,2,2},{1,0,0,1,-1,-1}}
local source={{31,0,0,255},{0,63,0,128},{0,0,31,0},{31,63,31,255}}
local function expected(m,left,top,right,bottom)
 d.clear('black')
 local det=m[1]*m[4]-m[2]*m[3]
 if det==0 then return end
 for y=top,bottom-1 do for x=left,right-1 do
  for v=0,1 do for u=0,1 do
   local dx=x+.5-(m[1]*u+m[3]*v+m[5])
   local dy=y+.5-(m[2]*u+m[4]*v+m[6])
   local U,V=(dx*m[4]-dy*m[3])/det,(dy*m[1]-dx*m[2])/det
   if U>=0 and U<1 and V>=0 and V<1 then
    local c=source[v*2+u+1]
    d.fill_rect(x,y,1,1,{r=((c[1]*c[4]+127)//255)*8,
     g=((c[2]*c[4]+127)//255)*4,b=((c[3]*c[4]+127)//255)*8})
   end
  end end
 end end
end
for _,m in ipairs(cases) do for clip=0,1 do
 local l,t,r,b=clip,clip,8-clip,8-clip
 expected(m,l,t,r,b);d.present({retained=true})
 row:load({1,table.unpack(m)});d.update_textures(batch,row)
 d.clear('black');d.draw_textures(batch,l,t,r,b)
 assert(d.present()==0,'independent texture oracle')
 actor:evaluate(m);assert(sk.update_textures(writer,actor)==sprites)
 d.clear('black');d.draw_textures(sprites,l,t,r,b)
 assert(d.present()==0,'skeleton texture oracle')
end end
-- f32 rows publish their rounded values through the same C sampler.
local row32=vm.buffer(7,'f32')
for _,m in ipairs(cases) do
 row32:load({1,table.unpack(m)})
 local rounded={};for i=1,6 do rounded[i]=row32:get(i+1) end
 expected(rounded,0,0,8,8);d.present({retained=true})
 d.update_textures(batch,row32);d.clear('black');d.draw_textures(batch)
 assert(d.present()==0,'f32 texture oracle')
end
local root={1,0,0,1,3,3}
actor:sample(1,500000,'clamp');actor:evaluate(root);sk.update_textures(writer,actor)
local m={math.cos(.5),math.sin(.5),-math.sin(.5),math.cos(.5),3,3}
expected(m,0,0,8,8);d.present({retained=true})
d.clear('black');d.draw_textures(sprites);assert(d.present()==0,'sampled rotation')
-- Ordered translucent overlap covers a joint; layer/visibility changes use
-- the same core item order, including ties resolved by part ID.
local red=d.texture(1,1,string.char(255,0,0,255))
local green=d.texture(1,1,string.char(0,255,0,128))
local overlap_def=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},
 parts={{1,1,0,1,0,0,0,1,1},{1,2,0,1,0,0,0,1,1}},clips={}}
local overlap=overlap_def:instance()
local ow,ob=sk.textures(overlap_def,{
 {texture=red,x=0,y=0,width=1,height=1,anchor_x=0,anchor_y=0},
 {texture=green,x=0,y=0,width=1,height=1,anchor_x=0,anchor_y=0}})
local states=vm.buffer(8)
for mode=1,3 do
 states:load({1,1,mode==2 and 1 or 0,1,2,2,0,mode==3 and 0 or 1})
 overlap:set_parts(states);overlap:evaluate(root);sk.update_textures(ow,overlap)
 d.clear('black');d.fill_rect(3,3,1,1,mode==1 and {r=120,g=128,b=0} or 'red')
 d.present({retained=true});d.clear('black');d.draw_textures(ob)
 assert(d.present()==0,'joint coverage/order/visibility')
end
-- Restore the sampled expected image for failure-preservation checks.
expected(m,0,0,8,8);d.present({retained=true})
bad(d.texture,0,1,'');bad(d.texture,1,1,'bad');bad(d.texture,'1',1,'abcd')
bad(d.texture_batch,{},4097);bad(d.update_textures,sprites,row)
bad(d.draw_textures,batch,0);bad(d.draw_textures,batch,0,0,9,8)
local two=vm.buffer(14);two:load({1,1,0,0,1,0,0,2,1,0,0,1,1,1})
bad(d.update_textures,batch,two)
local foreign=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},parts={},clips={}}:instance()
bad(sk.update_textures,writer,foreign)
local part=vm.buffer(4);part:load({1,2,0,1});actor:set_parts(part);actor:evaluate(root)
bad(sk.update_textures,writer,actor)
d.clear('black');d.draw_textures(sprites);assert(d.present()==0,'failed publish preservation')
part:load({1,1,0,1});actor:set_parts(part)
-- A source input cannot survive by accidental reliance on application refs.
collectgarbage('collect')
probe.oom(function() return d.texture(1,1,string.char(0,0,0,255)) end)
probe.oom(function() return sk.textures(def,{}) end)
local function steady()
 for i=1,10000 do
  actor:sample(1,i*13,'repeat');actor:evaluate(root);sk.update_textures(writer,actor)
  d.draw_textures(sprites,0,0,8,8)
 end
end
probe.noalloc(steady)
probe.measure('texture_sample_evaluate',function(i) actor:sample(1,i*33333,'repeat');actor:evaluate(root) end,2)
probe.measure('texture_adapter',function() sk.update_textures(writer,actor) end,1)
probe.measure('texture_raster_8x8',function() d.draw_textures(sprites) end,1)
probe.measure('texture_present_8x8',function() d.draw_textures(sprites);d.present() end,2)
local large_actor,large_writer,large_batch
-- Lua 5.5 long strings retain the creating allocator callback. Construct the
-- input before the temporary measurement hook; its bytes remain in the
-- measured baseline, and Display's copied texture is allocated under the hook.
local large_rgba=string.char(255,80,16,255):rep(1024)
probe.measure('texture_init_32x32_16parts',function()
 local texture=d.texture(32,32,large_rgba)
 local parts={};for i=1,16 do parts[i]={1,1,i,1,i,0,0,1,1} end
 local definition=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},parts=parts,clips={}}
 large_actor=definition:instance()
 large_writer,large_batch=sk.textures(definition,{{texture=texture,x=0,y=0,width=32,height=32,anchor_x=16,anchor_y=16}})
end,4,true)
probe.noalloc(function()
 for _=1,10000 do large_actor:evaluate(root);sk.update_textures(large_writer,large_actor);d.draw_textures(large_batch,0,0,8,8) end
end)
d.deinit();bad(d.draw_textures,batch)
print('TEXTURE Lua contract PASS frames=10000')
