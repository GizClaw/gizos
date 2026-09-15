local d = require('display')
local probe = require('raster_test')
if d.width == 240 then
    -- Test-only phase measurement; no production profiling API or second UI.
    local measure = probe.measure
    local geometry, colors, commands = {}, {{},{},{}}, {{},{},{}}
    measure('preprocess', function()
        for i=1,1536 do
            local x,y=(i-1)%48,math.floor((i-1)/48)
            local top,bottom=math.floor(y*240/32),math.floor((y+1)*240/32)
            geometry[i]={x=x*5,y=top,width=5,height=bottom-top,color_index=i}
            for p=1,3 do
                local t=(p-1)*128
                local function mix(a,b) return math.floor((a*(256-t)+b*t)/256+.5) end
                local color={r=mix(i%32,(i*3)%32)*8,g=mix((i+3)%64,(i+9)%64)*4,b=mix((i+7)%32,(i+21)%32)*8}
                colors[p][i]=color
                commands[p][i]={0,x*5,top,5,bottom-top,color}
            end
        end
    end,0,true)
    local batch,palettes,old = nil,{},{}
    measure('compile_new',function()
        batch=d.compile_rects(geometry)
        for p=1,3 do palettes[p]=d.compile_palette(colors[p]) end
    end,4,true)
    measure('compile_old_three_palettes',function()
        for p=1,3 do old[p]=d.compile_commands(commands[p]) end
    end,3,true)
    local output=d.compile_palette(colors[1])
    local function scalar(p)
        for i,r in ipairs(geometry) do d.fill_rect(r.x,r.y,r.width,r.height,colors[p][i]) end
    end
    measure('scalar_first_fill_damage',function() scalar(1) end,1536,true)
    measure('commands_first_fill_damage',function() d.draw_commands(old[1]) end,1,true)
    measure('new_first_fill_damage',function() d.draw_rects(batch,palettes[1]) end,1,true)
    d.clear('black');scalar(1);d.present({retained=true})
    d.draw_commands(old[1]);assert(d.present()==0)
    d.draw_rects(batch,palettes[1]);assert(d.present()==0)
    measure('scalar_transition_fill_damage',function(i) scalar(i%3+1) end,1536)
    measure('commands_transition_fill_damage',function(i) d.draw_commands(old[i%3+1]) end,1)
    measure('new_fill_damage',function(i) d.draw_rects(batch,palettes[i%3+1]) end,1)
    measure('palette_transition',function(i) d.blend_palette(output,palettes[1],palettes[3],(i%3)*128) end,1)
    measure('new_transition_fill_damage',function(i)
        d.blend_palette(output,palettes[1],palettes[3],(i%3)*128)
        d.draw_rects(batch,output)
    end,2)
    d.draw_rects(batch,palettes[1]);d.present()
    local background=d.capture_region(0,0,240,240)
    d.restore_background(background);d.present()
    measure('cached_background_steady',function() d.restore_background(background) end,1)
    local sprite=d.compile_rects({{x=0,y=0,width=8,height=8,color_index=1}})
    measure('foreground_and_restore',function()
        d.draw_rects(sprite,palettes[2]);d.restore_background(background)
    end,2)
    -- Fill once before each submission measurement; report the combined stage
    -- separately instead of attributing transfer work to the core.
    local present_options = {bounds=true}
    measure('new_fill_damage_and_present',function(i)
        d.draw_rects(batch,palettes[i%3+1]);d.present(present_options)
    end,2)
    d.deinit()
    return
end
local function bad(f, ...) assert(not pcall(f, ...)) end
local source = {
    {x=-2, y=1, width=7, height=4, color_index=1},
    {x=3, y=3, width=7, height=4, color_index=2},
}
local batch = d.compile_rects(source)
source[1].width = 0
local ca = {{r=255,g=0,b=0}, {r=0,g=0,b=255}}
local cb = {{r=0,g=255,b=0}, {r=255,g=255,b=0}}
local a, b = d.compile_palette(ca), d.compile_palette(cb)
local out = d.compile_palette({'black','black'})
local function mix(x,y,t,bits)
    x, y = x >> (8-bits), y >> (8-bits)
    return math.floor((x*(256-t)+y*t)/256+.5) << (8-bits)
end
for _, t in ipairs({0,1,127,128,255,256}) do
    d.clear('black')
    for i, r in ipairs({
        {x=1,y=1,width=4,height=4}, {x=3,y=3,width=4,height=3},
    }) do
        d.fill_rect(r.x,r.y,r.width,r.height,{
            r=mix(ca[i].r,cb[i].r,t,5),
            g=mix(ca[i].g,cb[i].g,t,6),
            b=mix(ca[i].b,cb[i].b,t,5),
        })
    end
    d.present({retained=true})
    d.clear('black')
    d.blend_palette(out,a,b,t)
    d.draw_rects(batch,out,1,0,7,6)
    assert(d.present()==0, 'replay oracle '..t)
end
bad(d.draw_rects,batch,out,0)
bad(d.draw_rects,batch,out,0,0,9,8)
bad(d.draw_rects,batch,out,4,0,3,8)
bad(d.blend_palette,out,a,b,257)
bad(d.blend_palette,out,a,b,-1)
bad(d.blend_palette,out,a,b,.5)
bad(d.blend_palette,out,a,d.compile_palette({}),128)
bad(d.compile_palette,{{255,0,0}})
bad(d.compile_rects,{{x=0,y=0,width=-1,height=1,color_index=1}})
bad(d.compile_rects,{{x=0,y=0,width=1,height=1,color_index=0}})
bad(d.compile_rects,{{x=0,y=0,width=1,height=1,color_index=1.5}})
bad(d.compile_rects,{[2]={x=0,y=0,width=1,height=1,color_index=1}})
local late_bad = d.compile_rects({
    {x=0,y=0,width=8,height=8,color_index=1},
    {x=100,y=100,width=0,height=0,color_index=3},
})
bad(d.draw_rects,late_bad,out)
assert(d.present()==0, 'late failure wrote pixels')
d.draw_rects(d.compile_rects({}),d.compile_palette({}))
d.blend_palette(d.compile_palette({}),d.compile_palette({}),d.compile_palette({}),256)
assert(d.present()==0)
-- In-place palette output and immutable source data.
d.blend_palette(a,a,b,256)
d.clear('black');d.draw_rects(batch,b);d.present()
d.clear('black');d.draw_rects(batch,a);assert(d.present()==0)
-- A full background restores rectangles drawn through the new path.
d.clear('white');d.present()
local background = d.capture_region(0,0,8,8)
d.restore_background(background);d.present()
d.draw_rects(batch,out);d.restore_background(background)
assert(d.present()==0, 'missing background damage')
local hot = function()
    for t=0,16 do
        d.blend_palette(out,a,b,t)
        d.draw_rects(batch,out)
    end
end
probe.noalloc(hot)
probe.oom(function() return d.compile_palette(ca) end)
probe.oom(function() return d.compile_rects(source) end)
collectgarbage('collect')
d.clear('black');d.draw_rects(batch,b);d.present()
-- Failed submission invalidates the retained baseline and retries all pixels.
for _, failure in ipairs({{1,false},{0,true}}) do
    d.clear('black');d.present({retained=true})
    d.draw_rects(batch,b)
    probe.fail(failure[1],failure[2])
    assert(not pcall(d.present))
    assert(d.present()==64)
    assert(d.present()==0)
end
-- A getter may close Display during compilation; no subsequent draw may use it.
local closing = setmetatable({}, {__index=function() d.deinit();return 0 end})
local unused = d.compile_palette({closing})
bad(d.draw_rects,batch,b)
d.blend_palette(unused,unused,unused,128)
