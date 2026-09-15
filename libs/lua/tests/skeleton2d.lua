local sk=require('skeleton2d')
local v=require('vmath')
local function fails(f,...) assert(not pcall(f,...)) end
local spec={schema_version=1,bones={{0,10,20,0,1,1},{1,20,0,0,1,1}},parts={{1,1,0,1,0,0,0,1,1},{2,1,1,1,0,0,0,1,1}},clips={{duration_us=1000000,tracks={{bone=1,channel=1,linear=true,shortest=false,keys={{0,10},{1000000,30}}}}}}}
local unsupported,message=pcall(sk.compile,{schema_version=2})
assert(not unsupported and message:find('skeleton2d.compile',1,true))
-- Empty clips still retain temporary userdata while parsing.
local empty_clips={}
for i=1,32 do empty_clips[i]={duration_us=1,tracks={}} end
local max_clips=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},parts={},clips=empty_clips}
max_clips:instance():sample(32,0,'clamp')
local def=sk.compile(spec)
local actor=def:instance()
local resources={{vertices={{-4,-4},{4,-4},{4,4},{-4,4}},primitives={{0,1,4,63488}}}}
local writer,mesh=sk.mesh(def,resources,{vertices=8,primitives=2})
local root={1,0,0,1,0,0}
local matrices,items,bounds=v.buffer(12),v.buffer(18),v.buffer(10)
fails(actor.copy_matrices,actor,matrices)
actor:sample(1,500000,'repeat','current');actor:evaluate(root)
actor:copy_matrices(matrices);assert(matrices:get(5)==20 and matrices:get(11)==40)
assert(sk.update_mesh(writer,actor)==mesh)
writer:copy_bounds(bounds);assert(bounds:get(2)==16 and bounds:get(4)==24)
spec.bones[1][2]=500;resources[1].vertices[1][1]=500
actor:sample(1,-250000,'repeat','current');actor:evaluate(root);actor:copy_matrices(matrices);assert(matrices:get(5)==25)
local parts=v.buffer(8);parts:load({1,1,2,1,2,1,-1,1});actor:set_parts(parts);actor:evaluate(root);actor:copy_draw_items(items);assert(items:get(1)==2)
parts:load({1,256,2,1,2,1,-1,1});actor:set_parts(parts);actor:evaluate(root);fails(sk.update_mesh,writer,actor);writer:copy_bounds(bounds);assert(bounds:get(2)==16)
parts:load({1,1,2,1,2,1,-1,1});actor:set_parts(parts)
fails(actor.blend,actor,0/0);fails(actor.blend,actor,1.1);fails(actor.sample,actor,1,0,'unknown','a')
fails(actor.evaluate,actor,{1e6,0,0,1e6,0,0})
fails(actor.copy_matrices,actor,v.buffer(1))
local tiny=sk.mesh(def,resources,{vertices=1,primitives=1});fails(sk.update_mesh,tiny,actor)
local other=sk.compile(spec):instance();other:evaluate(root);fails(sk.update_mesh,writer,other)
spec.bones[2][1]=2;fails(sk.compile,spec);spec.bones[2][1]=1
spec.clips[1].tracks[1].keys[2][1]=0;fails(sk.compile,spec)
local locals=v.buffer(12);locals:load({1,0,0,0,1,1,1,0,0,0,1,1});fails(actor.set_local,actor,locals)
assert(def:bytes()>0 and actor:bytes()>0 and writer:bytes()>0)
local override=v.buffer(6);override:load({2,20,0,0,1,1})
-- Both numeric storage kinds cross the double-precision core boundary.
for _,kind in ipairs({'f64','f32'}) do
 local fparts=v.buffer(8,kind);fparts:load({1,1,2,1,2,1,-1,1})
 local flocal=v.buffer(6,kind);flocal:load({2,20,0,0,1,1})
 actor:set_parts(fparts);actor:set_local(flocal);actor:evaluate(root)
 local fm,fi,fb=v.buffer(12,kind),v.buffer(18,kind),v.buffer(10,kind)
 actor:copy_matrices(fm);actor:copy_matrices(matrices)
 actor:copy_draw_items(fi);actor:copy_draw_items(items)
 sk.update_mesh(writer,actor);writer:copy_bounds(fb);writer:copy_bounds(bounds)
 for i=1,12 do assert(math.abs(fm:get(i)-matrices:get(i))<.001) end
 for i=1,18 do assert(math.abs(fi:get(i)-items:get(i))<.001) end
 for i=1,10 do assert(math.abs(fb:get(i)-bounds:get(i))<.001) end
end
-- Exercise the production Display resource owner without acquiring a Display;
-- the C harness measures the entire VM from creation through zero at close.
local texture=test_texture_new(32,32,string.char(255,80,16,128):rep(1024))
local tw,tb=sk.textures(def,{{texture=texture,x=0,y=0,width=32,height=32,anchor_x=16,anchor_y=16}})
texture=nil;collectgarbage('collect')
-- Reused closure, tables, buffers and output mesh in a 10000-frame steady loop.
return function()
 for i=1,10000 do
  actor:sample(1,i*33333,'repeat','a');actor:sample(1,i*33333+200000,'repeat','b');actor:blend(.5)
  actor:set_parts(parts);actor:set_local(override)
  actor:evaluate(root);sk.update_mesh(writer,actor);assert(sk.update_textures(tw,actor)==tb)
  actor:copy_matrices(matrices);actor:copy_draw_items(items);writer:copy_bounds(bounds)
 end
end
