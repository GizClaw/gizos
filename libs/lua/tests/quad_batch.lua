local d = require('display')
local p = require('raster_test')
local function bad(f, ...) assert(not pcall(f, ...)) end
local colors = {'red','blue','white',{r=23,g=187,b=91}}
local palette = d.compile_palette(colors)
local entries = {{0,1,1},{0,.52,2},{.48,1,3},{.2,.8,4,0,1},
                 {0,.52,1,.2,.8},{.48,1,2,.2,.8},
                 {.2,.8,3,.2,.8},{.5,.5,4},{0,1,2,.5,.5}}
local batch = d.compile_quad_batch(entries)
local function draw(handle, c, q)
    d.draw_quad_batch(handle,c,table.unpack(q))
end
-- Deliberately separate transverse interpolation from strip interpolation;
-- this is the existing scalar polygon contract, including degenerate spans.
local patch, strip = {{}, {}, {}, {}}, {{}, {}, {}, {}}
local corners={{},{},{},{}}
local function scalar(records, c, q)
    for i=1,4 do corners[i][1],corners[i][2]=q[i*2-1],q[i*2] end
    for _, r in ipairs(records) do
        local a,b,cc,dd = table.unpack(corners)
        if r[4] then
            for axis=1,2 do
                patch[1][axis]=a[axis]+(dd[axis]-a[axis])*r[4]
                patch[2][axis]=b[axis]+(cc[axis]-b[axis])*r[4]
                patch[3][axis]=b[axis]+(cc[axis]-b[axis])*r[5]
                patch[4][axis]=a[axis]+(dd[axis]-a[axis])*r[5]
            end
            a,b,cc,dd=table.unpack(patch)
        end
        for axis=1,2 do
            local ab,dc=b[axis]-a[axis],cc[axis]-dd[axis]
            strip[1][axis]=a[axis]+ab*r[1]
            strip[2][axis]=a[axis]+ab*r[2]
            strip[3][axis]=dd[axis]+dc*r[2]
            strip[4][axis]=dd[axis]+dc*r[1]
        end
        d.fill_polygon(strip,c[r[3]])
    end
end
local function compare(records, handle, q, cs, pal)
    cs,pal=cs or colors,pal or palette
    d.clear('black');scalar(records,cs,q);d.present({retained=true})
    d.clear('black');draw(handle,cs,q);assert(d.present()==0,'table colors pixels')
    d.clear('black');draw(handle,pal,q);assert(d.present()==0,'palette pixels')
end
for _,q in ipairs({{0,0,7,0,7,7,0,7}, {-9,-3,12,1,6,13,-2,6},
    {0,0,7,7,0,7,7,0}, {0,0,7,0,3,3,0,7}, {4,4,4,4,4,4,4,4},
    {-100000,-100000,100000,-100000,100000,100000,-100000,100000},
    {900,900,910,900,910,910,900,910}}) do compare(entries,batch,q) end
local seed=137
local function random()
    seed=(seed*1664525+1013904223)%4294967296
    return seed/4294967296
end
-- Moving skewed/concave quads, crossings, shared edges and near-integer rows.
for frame=1,240 do
    local q={}
    for i=1,8 do q[i]=(random()*2-1)*14+4 end
    if frame%3==0 then
        q[2]=math.floor(q[2])+(frame%2==0 and 1e-12 or -1e-12)
        q[4]=q[2]
    end
    compare(entries,batch,q)
end
local q={1,1,6,1,6,7,1,7}
local copied=d.compile_quad_batch({{0,1,1}})
local source={{0,1,1}}
local immutable=d.compile_quad_batch(source)
source[1][1]=1
compare({{0,1,1}},immutable,q)
-- Reused color inputs are sampled afresh on each draw.
local changing={{r=255,g=0,b=0}}
local mutable=d.compile_palette(changing)
local blue=d.compile_palette({'blue'})
draw(copied,changing,q);d.present({retained=true})
changing[1].r,changing[1].b=0,255
d.clear('black');draw(copied,changing,q);d.present()
d.blend_palette(mutable,mutable,blue,256)
d.clear('black');draw(copied,mutable,q);assert(d.present()==0)
local empty=d.compile_quad_batch({})
d.clear('black');d.present({retained=true});draw(empty,{},q);assert(d.present()==0)
local full={}
for i=1,256 do full[i]={0,1,256} end
local maxbatch=d.compile_quad_batch(full)
local maxcolors={}
for i=1,256 do maxcolors[i]='blue' end
p.noalloc(function() draw(maxbatch,maxcolors,q) end)
full[257]={0,1,1};bad(d.compile_quad_batch,full)
for _,records in ipairs({{{-.01,1,1}},{{0,1.01,1}},{{.8,.2,1}},{{0,1,0}},
    {{0,1,257}},{{0,1,1.5}},{{0,1,1,.8,.2}},{{0,1,1,0}},
    {{0,1,1,0,0/0}},{{0,math.huge,1}},{{0,1}},{{0,1,1,0,1,0}},
    {oops={0,1,1}},{{0,1,1},false}}) do bad(d.compile_quad_batch,records) end
-- Failed late color/coordinate/index validation cannot partially draw.
d.clear('black');d.present({retained=true})
for _,c in ipairs({{}, {'red','blue','white',false}, {'red','blue','white',{r=256,g=0,b=0}}}) do
    bad(draw,batch,c,q);assert(d.present()==0)
end
bad(draw,batch,d.compile_palette({'red'}),q);assert(d.present()==0)
for _,v in ipairs({100001,-100001,math.huge,0/0}) do
    local invalid={table.unpack(q)};invalid[8]=v
    bad(draw,batch,colors,invalid);assert(d.present()==0)
end
bad(d.draw_quad_batch,batch,colors,1,2)
bad(draw,{},colors,q)
p.oom(function() d.compile_quad_batch(entries) end)
collectgarbage('collect')
p.noalloc(function() draw(batch,palette,q) end)
p.noalloc(function() draw(batch,colors,q) end)
-- Reentrant RGB getters use independent per-call scratch.
local reentrant=setmetatable({}, {__index=function(_,key)
    draw(copied,{'blue'},q)
    collectgarbage('collect')
    return key=='r' and 255 or 0
end})
d.clear('black');draw(copied,{reentrant},q);d.present({retained=true})
d.clear('black');draw(copied,{'red'},q);assert(d.present()==0)
local bg=d.capture_region(0,0,d.width,d.height)
d.restore_background(bg);d.present()
draw(batch,colors,q);d.restore_background(bg);assert(d.present()==0,'background damage')
-- Constructor failure/GC releases all VM storage; no native registry roots.
collectgarbage('collect');local before=collectgarbage('count')
for i=1,30 do local unused=d.compile_quad_batch(entries) end
collectgarbage('collect');assert(collectgarbage('count')<before+2)
if d.width==240 then
    local records, dynamic={},{}
    for band=1,9 do
        for level=1,6 do
            local index=#records+1
            records[index]={0,.55/level,index,(band-1)/9,band/9}
            dynamic[index]={r=index*4,g=120+index,b=250-index}
        end
    end
    local prepared
    local fixed=d.compile_palette(dynamic)
    p.measure('quad_compile_54',function() prepared=d.compile_quad_batch(records) end,1,true)
    local moving={}
    local function place(i)
        local t=i*.007
        moving[1],moving[2]=15+t,10-t
        moving[3],moving[4]=190+t,70+t
        moving[5],moving[6]=150-t,220-t
        moving[7],moving[8]=65+t,165+t
    end
    -- The scalar baseline prepares each patch and its edge deltas once for
    -- six widths, matching applications that already hoist that arithmetic.
    local edge={}
    local function reference(i)
        place(i)
        for band=1,9 do
            local top,bottom=(band-1)/9,band/9
            for axis=1,2 do
                local a,b,c,dd=moving[axis],moving[2+axis],moving[4+axis],moving[6+axis]
                local aa,bb=a+(dd-a)*top,b+(c-b)*top
                local cc,d0=b+(c-b)*bottom,a+(dd-a)*bottom
                edge[axis],edge[2+axis],edge[4+axis],edge[6+axis]=aa,bb-aa,d0,cc-d0
            end
            for level=1,6 do
                local r=records[(band-1)*6+level]
                for axis=1,2 do
                    strip[1][axis]=edge[axis]+edge[2+axis]*r[1]
                    strip[2][axis]=edge[axis]+edge[2+axis]*r[2]
                    strip[3][axis]=edge[4+axis]+edge[6+axis]*r[2]
                    strip[4][axis]=edge[4+axis]+edge[6+axis]*r[1]
                end
                d.fill_polygon(strip,dynamic[r[3]])
            end
        end
    end
    local function tables(i) place(i);draw(prepared,dynamic,moving) end
    local function native(i) place(i);draw(prepared,fixed,moving) end
    p.measure('quad_scalar_54',reference,54)
    p.measure('quad_table_colors_54',tables,1)
    p.measure('quad_palette_54',native,1)
    for frame=1,40 do
        place(frame);compare(records,prepared,moving,dynamic,fixed)
        d.clear('black');reference(frame);assert(d.present()==0,'hoisted scalar pixels')
    end
end
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(draw,copied,{closing},q)
bad(draw,copied,palette,q)
d.deinit()
local detached=d.compile_quad_batch(entries)
assert(detached)
