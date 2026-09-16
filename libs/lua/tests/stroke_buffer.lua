local d,v=require('display'),require('vmath')
local probe=require('raster_test')
local function b(t) local r=v.buffer(#t);r:load(t);return r end
local function bad(...) assert(not pcall(...)) end
local xy=b{1,1, 4,5, 7,2}
local descriptor={buffer=xy,count=3}
local points={{1,1},{4,5},{7,2}}
local widths,colors={1,2},{'red','blue'}
local function draw(p,fast,smooth,scale,tolerance)
  return d.stroke_path(p,widths,colors,.25,1,7,true,fast,smooth,scale,tolerance)
end
for _,fast in ipairs({false,true}) do
 for _,smooth in ipairs({false,true}) do
  for _,scale in ipairs({.8,1,1.2}) do
   for _,tolerance in ipairs({0,.1,.25}) do
    for k=1,12 do
     -- Moving, clipped, reversed and degenerate paths share the old raster.
     for i=1,3 do
      local x,y=(k*i)%13-3,(k+i)%11-2
      if k%4==0 then x,y=3,3 end
      points[i][1],points[i][2]=x,y;xy:set(2*i-1,x);xy:set(2*i,y)
     end
     d.clear('black');draw(points,fast,smooth,scale,tolerance);d.present({retained=true})
     for _=1,2 do
      d.clear('black');draw(descriptor,fast,smooth,scale,tolerance)
      assert(d.present()==0,'packed stroke pixels')
     end
    end
   end
  end
 end
end
xy:load{1,1,4,5,7,2}
local function cached() return d.stroke_path(descriptor,widths,colors,0,0,8,true,true) end
assert(not cached());assert(cached())
xy:set(1,2);assert(not cached());assert(cached())
-- Buffer identity alone is neither sufficient nor necessary for a cache hit.
descriptor.buffer=b{2,1,4,5,7,2};assert(cached())
descriptor.buffer:set(6,3);assert(not cached());assert(cached())
widths[2]=3;assert(not cached());colors[2]='green';assert(not cached())
descriptor.count=2;widths[2],colors[2]=nil,nil
assert(not cached());assert(cached())
-- Default positional arguments work with the rooted descriptor on the stack.
d.stroke_path(descriptor,widths,'red')
d.present({retained=true});assert(cached());d.present()
local rejected=0
local function rejects(p,w,c)
 rejected=rejected+1
 bad(d.stroke_path,p,w or widths,c or colors,0,0,8,true,true)
 assert(d.present()==0,'invalid stroke wrote pixels '..rejected)
end
for _,count in ipairs({0,1,257,1.5,'2'}) do rejects({buffer=xy,count=count}) end
rejects({buffer=xy});rejects({count=2});rejects({buffer=false,count=2})
rejects({buffer=v.buffer(4,'f32'),count=2})
rejects({buffer=v.buffer(3),count=2})
rejects({buffer=xy,count=2,{1,1}})
rejects({buffer=xy,count=2,[99]={1,1}})
rejects({buffer=b{1,1,4,100001},count=2})
rejects(descriptor,{-1});rejects(descriptor,{1},{'bad color'})
assert(cached(),'failed input invalidated span cache')
probe.noalloc(cached)
probe.noalloc(function() d.stroke_path(descriptor,widths,'red',0,0,8,false,true,true,1,.1) end)
d.present()
-- Descriptor fields are raw; a metatable cannot supply the missing buffer.
rejects(setmetatable({count=2},{__index=function() error('descriptor getter') end}))
-- Color getters may drop/replace descriptor references and collect the original
-- buffer. It remains rooted until coordinates have been decoded.
local mutating={buffer=b{1,1,6,6},count=2}
local color=setmetatable({}, {__index=function()
 mutating.buffer=nil;collectgarbage('collect');return 255
end})
d.clear('black');d.stroke_path({{1,1},{6,6}},{1},'white');d.present()
d.clear('black');d.stroke_path(mutating,{1},color);assert(d.present()==0)
-- Bounded maxima and zero-width geometry retain the same output.
do
 local all,packed,w={},{},{}
 for i=1,256 do all[i]={i%8,i%7};packed[2*i-1]=i%8;packed[2*i]=i%7;if i<256 then w[i]=.25 end end
 d.clear('black');d.stroke_path(all,w,'red');d.present()
 d.clear('black');d.stroke_path({buffer=b(packed),count=256},w,'red');assert(d.present()==0)
end
-- First cache allocation can fail without publishing pixels; retry is valid.
local fresh={buffer=b{1,1,6,6},count=2};local fw={1}
d.present();probe.oom(function() d.stroke_path(fresh,fw,'red',0,0,8,true,true) end)
assert(d.present()==0);d.stroke_path(fresh,fw,'red',0,0,8,true,true)
local weak=setmetatable({fresh,fresh.buffer,fw},{__mode='v'})
fresh,fw=nil,nil;collectgarbage('collect');assert(not weak[1] and not weak[2] and not weak[3])
-- Color-triggered finalizers may release Display; the old recheck must still guard
-- publication. Reacquisition belongs to the host; no stale writes are allowed.
local held=setmetatable({}, {__gc=function() d.deinit() end})
local closing=setmetatable({}, {__index=function() held=nil;collectgarbage('collect');return 0 end})
bad(d.stroke_path,descriptor,widths,closing,0,0,8,true,true)
bad(cached)
