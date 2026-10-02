local d=require('display')
local p=require('raster_test')
local colors={'red','white','blue','green'}
local palette=d.compile_palette(colors)
local entries={{0,1,1},{0,.52,2},{.1,.4,3,.15,.87},{.25,.36,4,.2,.7}}
local batch=d.compile_quad_batch(entries)
local material=d.compile_quad_material(batch)
-- Lua expands only the last expression; keep the coordinate call explicit.
local function paint(f,m,c,q,top,bottom)
    return f(m,c,q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],top,bottom)
end
-- Independent inverse bilinear solver; the production scan converter does
-- neither Newton iteration nor per-pixel parameter inversion.
local function reference(q,top,bottom)
    local ex,ey=q[3]-q[1],q[4]-q[2]
    local dx,dy=q[7]-q[1],q[8]-q[2]
    local kx,ky=q[5]-q[7]-ex,q[6]-q[8]-ey
    for y=top,bottom-1 do for x=0,63 do
        local u,v=.5,.5
        for _=1,12 do
            local rx=q[1]+u*ex+v*dx+u*v*kx-x
            local ry=q[2]+u*ey+v*dy+u*v*ky-y
            local ax,ay=ex+v*kx,ey+v*ky
            local bx,by=dx+u*kx,dy+u*ky
            local det=ax*by-ay*bx
            if math.abs(det)<1e-12 then break end
            u=u-(rx*by-ry*bx)/det
            v=v-(ax*ry-ay*rx)/det
        end
        local owner
        if u>=0 and u<1 and v>=0 and v<1 then
            for _,r in ipairs(entries) do
                if u>=r[1] and u<r[2] and v>=(r[4] or 0) and v<(r[5] or 1) then owner=r[3] end
            end
        end
        if owner then d.fill_rect(x,y,1,1,colors[owner]) end
    end end
end
local quads={
    {3.17,2.13,58.37,8.19,48.31,58.71,7.23,53.29},
    {7.23,53.29,48.31,58.71,58.37,8.19,3.17,2.13},
    {-20.13,-12.19,50.23,5.11,54.31,47.17,-15.29,57.13},
    {8.13,5.17,51.37,5.17,43.41,54.29,13.27,54.29},
}
for angle=1,12 do
    local co,si=math.cos(angle*.419),math.sin(angle*.419)
    local q={-22.137,-19.173,22.211,-19.173,17.197,19.257,-17.139,19.257}
    for i=1,8,2 do local x,y=q[i],q[i+1];q[i],q[i+1]=32.137+x*co-y*si,32.193+x*si+y*co end
    quads[#quads+1]=q
end
for _,q in ipairs(quads) do
    d.clear('black');reference(q,0,64);d.present({retained=true})
    for _,c in ipairs({colors,palette}) do
        d.clear('black');assert(paint(d.draw_quad_material,material,c,q,0,64))
        assert(d.present()==0,'independent inverse bilinear oracle')
        d.clear('black')
        for y=63,0,-1 do assert(paint(d.draw_quad_material,material,c,q,y,y+1)) end
        assert(d.present()==0,'clip composition')
        p.noalloc(function() paint(d.draw_quad_material,material,c,q,0,64) end)
    end
    d.clear('green');local bg=d.capture_region(0,0,d.width,d.height)
    d.restore_background(bg);d.present()
    paint(d.draw_quad_material,material,palette,q,5,50);d.restore_background(bg)
    assert(d.present()==0,'material damage restoration')
end
-- Sparse parameter bounds and holes must retain the independent pixel rule.
-- These exercise clipping to material support rather than only the full face.
local saved_entries=entries
for _,recipe in ipairs({{{.11,.39,1,.17,.43},{.61,.89,2,.57,.83}},
                        {{0,.52,1},{0,.065,2},{0,.026,3}},
                        {{.48,1,1},{.85,1,2},{.94,1,3}},
                        {{.5,.5,1},{0,1,2,.3,.3}}}) do
    entries=recipe
    local sparse=d.compile_quad_material(d.compile_quad_batch(entries))
    for i=1,4 do
        local q=quads[i]
        d.clear('black');reference(q,0,64);d.present({retained=true})
        d.clear('black');paint(d.draw_quad_material,sparse,palette,q,0,64)
        assert(d.present()==0,'sparse material support')
        d.clear('black')
        for row=63,0,-1 do paint(d.draw_quad_material,sparse,palette,q,row,row+1) end
        assert(d.present()==0,'sparse clip composition')
    end
end
entries=saved_entries

-- Non-convex and degenerate geometry replays the original batch unchanged.
for _,q in ipairs({{2,2,55,55,2,55,55,2},{2,2,55,2,20,15,2,55},{8,0,8,0,8,55,8,55},
    {8,20,55,20,55,20,8,20},
    {-100000,1,100000,1+1e-8,100000,2+1e-8,-100000,2}}) do
    d.clear('black');paint(d.draw_quad_batch,batch,palette,q);d.present({retained=true})
    d.clear('black');assert(not paint(d.draw_quad_material,material,palette,q))
    assert(d.present()==0,'fallback raster')
    d.clear('black');paint(d.draw_quad_batch,batch,palette,q,10,21);d.present()
    d.clear('black');assert(not paint(d.draw_quad_material,material,palette,q,10,21))
    assert(d.present()==0,'fallback row clip')
    local calls=0
    local c=setmetatable({}, {__index=function(_,key) calls=calls+1;return 0 end})
    paint(d.draw_quad_material,material,{c,c,c,c},q)
    assert(calls==12,'fallback decodes colors once')
end
-- A dense nested material case measures the complete public draw call.
local nested,cs={},{}
for band=1,9 do
    for level,width in ipairs({.52,.40,.29,.20,.12,.065}) do
        local i=#nested+1
        nested[i]={0,width,i,(band-1)/9,band/9}
        cs[i]={r=(i*17)%256,g=(i*31)%256,b=(i*43)%256}
    end
end
local original=d.compile_quad_batch(nested)
p.measure('material_compile_54',function() d.compile_quad_material(original) end,1,true)
local field=d.compile_quad_material(original)
local cp=d.compile_palette(cs)
local moving={}
local sample_quad={-140,-100,140,-100,110,100,-110,100}
local function place(i)
    local a=i*.014
    local co,si=math.cos(a),math.sin(a)
    local q=sample_quad
    for j=1,8,2 do moving[j]=120+q[j]*co-q[j+1]*si;moving[j+1]=120+q[j]*si+q[j+1]*co end
end
p.measure('material_batch_54',function(i) place(i);paint(d.draw_quad_batch,original,cp,moving) end,1)
p.measure('material_scan_54',function(i) place(i);paint(d.draw_quad_material,field,cp,moving) end,1)
local empty=d.compile_quad_material(d.compile_quad_batch({}))
d.clear('green');d.present({retained=true})
paint(d.draw_quad_material,empty,{},quads[1]);assert(d.present()==0)
local function bad(f,...) assert(not pcall(f,...)) end
bad(d.compile_quad_material,{})
bad(paint,d.draw_quad_material,material,{'red'},quads[1])
bad(paint,d.draw_quad_material,material,palette,quads[1],5,4)
bad(paint,d.draw_quad_material,material,palette,{0,0,1,0,1,1,0,math.huge})
local many={}
for i=1,32 do many[i]={(i-1)/32,i/32,1} end
bad(d.compile_quad_material,d.compile_quad_batch(many))
many={}
for i=1,16 do many[#many+1]={(i-1)/16,i/16,1};many[#many+1]={0,1,1,(i-1)/16,i/16} end
-- 16x16 is the maximum accepted grid; an extra interior knot exceeds it.
assert(d.compile_quad_material(d.compile_quad_batch(many)))
many[#many+1]={.01,.02,1}
bad(d.compile_quad_material,d.compile_quad_batch(many))
-- Integer grid boundaries are deliberately half-open, including reflected
-- quads. This oracle uses direct parameter arithmetic, not scan events.
local exact_entries={{0,1,1},{.25,.75,2,.25,.75},{.5,.5,3},{0,1,4,.5,.5}}
local exact=d.compile_quad_material(d.compile_quad_batch(exact_entries))
for _,q in ipairs({{0,0,64,0,64,64,0,64},{64,0,0,0,0,64,64,64},
                    {0,64,64,64,64,0,0,0}}) do
    d.clear('black')
    for y=0,64 do for x=0,64 do
        local u=(x-q[1])/(q[3]-q[1])
        local v=(y-q[2])/(q[8]-q[2])
        local owner
        for _,r in ipairs(exact_entries) do
            if u>=r[1] and u<r[2] and v>=(r[4] or 0) and v<(r[5] or 1) then owner=r[3] end
        end
        if owner then d.fill_rect(x,y,1,1,colors[owner]) end
    end end
    d.present({retained=true});d.clear('black')
    paint(d.draw_quad_material,exact,palette,q)
    assert(d.present()==0,'integer boundary ownership '..table.concat(q,','))
end
-- Validate the whole call before writing, even for empty clips.
d.clear('green');d.present({retained=true})
for _,clip in ipairs({{-1,10},{0,241},{10,9},{.5,1},{0,math.huge}}) do
    bad(paint,d.draw_quad_material,material,palette,quads[1],clip[1],clip[2])
    assert(d.present()==0)
end
bad(paint,d.draw_quad_material,material,{'red'},quads[1],8,8)
p.oom(function() d.compile_quad_material(batch) end)
-- The material pins its source only through ordinary userdata ownership.
local retained=d.compile_quad_material(d.compile_quad_batch({{0,1,1}}))
collectgarbage('collect')
local concave={2,2,55,2,20,15,2,55}
assert(not paint(d.draw_quad_material,retained,colors,concave))
p.noalloc(function() paint(d.draw_quad_material,retained,palette,concave) end)
local calls=0
local reentrant=setmetatable({}, {__index=function()
    calls=calls+1
    paint(d.draw_quad_material,exact,palette,quads[2],0,0)
    collectgarbage('collect')
    return 0
end})
paint(d.draw_quad_material,retained,{reentrant},quads[1],0,0)
assert(calls==3)
-- Saturate the actual 2 MiB VM limit, then verify GC recovers the storage.
collectgarbage('collect')
local before=collectgarbage('count')
local held,exhausted={},false
for i=1,5000 do
    local ok,value=pcall(d.compile_quad_material,batch)
    if not ok then assert(value=='not enough memory');exhausted=true;break end
    held[i]=value
end
assert(exhausted,'material must count against VM quota')
held=nil;collectgarbage('collect')
assert(collectgarbage('count')<before+8,'material storage must be collectible')
assert(d.compile_quad_material(batch))
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(paint,d.draw_quad_material,material,{closing,closing,closing,closing},quads[1])
