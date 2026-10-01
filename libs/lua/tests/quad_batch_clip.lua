local d=require('display')
local p=require('raster_test')
local function bad(f,...) assert(not pcall(f,...)) end
local colors={'red','white',{r=53,g=189,b=241},'blue'}
local palette=d.compile_palette(colors)
local entries={{0,1,1},{0,.5,2},{.5,1,3},{.2,.8,4,.1,.9},
               {0,.5,2,.1,.9},{.5,1,3,.1,.9},{.5,.5,4},{0,1,1,.5,.5}}
local batch=d.compile_quad_batch(entries)
local function draw(b,c,q,...)
    d.draw_quad_batch(b,c,q[1][1],q[1][2],q[2][1],q[2][2],
                     q[3][1],q[3][2],q[4][1],q[4][2],...)
end
local function reference(q,top,bottom)
    for _,r in ipairs(entries) do
        local a,b,c,dd=table.unpack(q)
        if r[4] then
            local patch={{},{},{},{}}
            for axis=1,2 do
                patch[1][axis]=a[axis]+(dd[axis]-a[axis])*r[4]
                patch[2][axis]=b[axis]+(c[axis]-b[axis])*r[4]
                patch[3][axis]=b[axis]+(c[axis]-b[axis])*r[5]
                patch[4][axis]=a[axis]+(dd[axis]-a[axis])*r[5]
            end
            a,b,c,dd=table.unpack(patch)
        end
        local strip={{},{},{},{}}
        for axis=1,2 do
            local ab,dc=b[axis]-a[axis],c[axis]-dd[axis]
            strip[1][axis]=a[axis]+ab*r[1]
            strip[2][axis]=a[axis]+ab*r[2]
            strip[3][axis]=dd[axis]+dc*r[2]
            strip[4][axis]=dd[axis]+dc*r[1]
        end
        d.fill_polygon(strip,colors[r[3]],0,top,bottom)
    end
end
local quads={
    {{0,0},{239,0},{239,240},{0,240}},
    {{20,15-1e-12},{220,16+1e-12},{190,225},{40,230}},
    {{40,230},{190,225},{220,16+1e-12},{20,15-1e-12}},
    {{-60,-80},{290,12},{220,290},{-40,160}},
    {{-100000,-100000},{100000,-100000},{100000,100000},{-100000,100000}},
    {{900,900},{910,900},{910,910},{900,910}},
    {{0,0},{239,240},{0,240},{239,0}},
    {{0,0},{230,16},{80,80},{0,230}},
    {{80,16},{80,16},{80,16},{80,16}},
    {{30,16},{190,16},{190,16},{30,16}},
    {{80,0},{80,0},{80,240},{80,240}},
}
local seed=823
for frame=1,24 do
    local q={}
    for i=1,4 do
        seed=(seed*1664525+1013904223)%4294967296
        local x=seed/4294967296*340-50
        seed=(seed*1664525+1013904223)%4294967296
        q[i]={x,seed/4294967296*340-50}
    end
    quads[#quads+1]=q
end
local widths={1,15,16,17,31,3}
for _,q in ipairs(quads) do
    -- Entire surface: overlapping primitives keep their painter order on each
    -- row, even when disjoint clips are visited in descending order.
    d.clear('black');reference(q);d.present({retained=true})
    for _,c in ipairs({colors,palette}) do
        for mode=1,3 do
            d.clear('black')
            local bottom,part=d.height,1
            while bottom>0 do
                local width=mode==1 and 1 or mode==2 and 16 or widths[(part-1)%#widths+1]
                local top=math.max(0,bottom-width)
                draw(batch,c,q,top,bottom)
                bottom,part=top,part+1
            end
            assert(d.present()==0,'disjoint clip composition')
        end
        d.clear('black');draw(batch,c,q);assert(d.present()==0,'legacy ten arguments')
        d.clear('black');draw(batch,c,q,nil,nil);assert(d.present()==0,'nil defaults')
    end
    -- Direct clipped scalar comparison also checks that rows OUTSIDE a clip
    -- are untouched; composition alone cannot catch an ignored clip.
    for _,clip in ipairs({{0,0},{0,1},{15,16},{16,17},{16,32},
                           {31,33},{239,240},{240,240},{17,211}}) do
        d.clear('green');reference(q,clip[1],clip[2]);d.present()
        for _,c in ipairs({colors,palette}) do
            d.clear('green');draw(batch,c,q,clip[1],clip[2])
            assert(d.present()==0,'clip membership or edge rounding')
        end
    end
end
local q=quads[2]
for _,c in ipairs({colors,palette}) do
    d.clear('black');reference(q,16);d.present({retained=true})
    d.clear('black');draw(batch,c,q,16);assert(d.present()==0,'top-only default')
    d.clear('black');draw(batch,c,q,16,nil);assert(d.present()==0,'nil bottom')
    d.clear('black');reference(q,nil,32);d.present()
    d.clear('black');draw(batch,c,q,nil,32);assert(d.present()==0,'nil top')
    p.noalloc(function() draw(batch,c,q,16,32) end)
    p.noalloc(function() draw(batch,c,q,16,16) end)
end
local empty=d.compile_quad_batch({})
d.clear('green');d.present({retained=true})
for _,clip in ipairs({{-1,32},{0,241},{32,16},{.5,32},{0,31.5},
    {0/0,32},{0,0/0},{math.huge,32},{0,math.huge},
    {math.maxinteger,math.maxinteger},{0,math.maxinteger},{math.mininteger,32}}) do
    for _,c in ipairs({colors,palette}) do
        bad(draw,batch,c,q,clip[1],clip[2]);assert(d.present()==0)
    end
    bad(draw,empty,{},q,clip[1],clip[2]);assert(d.present()==0)
end
bad(draw,batch,colors,q,0,32,1);assert(d.present()==0)
-- Empty clips still validate color, vertex, palette and acquisition state.
local invalid={{0,0},{1,0},{1,1},{0,math.huge}}
bad(draw,batch,palette,invalid,16,16)
bad(draw,batch,{'red','white','blue',false},q,16,16)
bad(draw,batch,d.compile_palette({'red'}),q,16,16)
assert(d.present()==0)
local calls=0
local getter=setmetatable({}, {__index=function(_,key)
    calls=calls+1
    draw(batch,palette,q,16,16)
    collectgarbage('collect')
    return key=='g' and 128 or 0
end})
local single=d.compile_quad_batch({{0,1,1}})
draw(single,{getter},q,16,16);assert(calls==3 and d.present()==0)
-- Nested drawing cannot corrupt the outer clip or its colors.
local nested=setmetatable({}, {__index=function(_,key)
    draw(single,d.compile_palette({'blue'}),q,16,32)
    return key=='r' and 255 or 0
end})
d.clear('black');draw(single,{nested},q,32,48);d.present()
d.clear('black');d.fill_polygon(q,'blue',0,16,32);d.fill_polygon(q,'red',0,32,48)
assert(d.present()==0,'reentrant clip scratch')
d.clear('green');local bg=d.capture_region(0,0,d.width,d.height)
d.restore_background(bg);d.present()
for _,c in ipairs({colors,palette}) do
    draw(batch,c,q,16,32);d.restore_background(bg)
    assert(d.present()==0,'clipped background damage')
end
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(draw,single,{closing},q,16,16)
bad(draw,batch,palette,q,16,32)
bad(draw,empty,{},q,16,16)
