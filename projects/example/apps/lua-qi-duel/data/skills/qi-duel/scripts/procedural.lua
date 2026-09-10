-- All remaining surfaces are geometry authored and animated by Lua.
-- H2VG is a bounded path command stream, not a bitmap or a sprite sheet.
local M={}
local pack=string.pack
local TAU=math.pi*2
local C={cyan=0x43ddff,purple=0xd257ff,orange=0xffa445,green=0x66ffc1,white=0xeaffff,dark=0x071321}
local function op(n,...) local v={...};return string.char(n)..pack(string.rep('f',#v),table.unpack(v)) end
local function paint(color,stroke,width,alpha)
    return pack('<BBi2BBBBff',10,stroke and 1 or 0,-1,(color>>16)&255,(color>>8)&255,color&255,255,width or 1,math.max(0,math.min(1,alpha or 1)))
end
local function builder(w,h)
    local p={'H2VG'..pack('<I2I2I2I2',w or 368,h or 448,0,1)}
    local b={}
    function b.raw(s) p[#p+1]=s end
    function b.path(points,closed)
        b.raw(op(4));for i,v in ipairs(points) do b.raw(op(i==1 and 5 or 6,v[1],v[2])) end
        if closed then b.raw(op(8)) end
    end
    function b.shape(points,fill,edge,width,alpha)
        b.path(points,true);if fill then b.raw(paint(fill,false,1,alpha)) end
        if edge then b.raw(paint(edge,true,width or .8,alpha)) end
    end
    function b.line(points,width,color,alpha) b.path(points,false);b.raw(paint(color,true,width,alpha)) end
    function b.neon(points,width,color,alpha)
        alpha=alpha or 1
        b.line(points,width*5,color,alpha*.035);b.line(points,width*2.5,color,alpha*.13)
        b.line(points,width,color,alpha*.85);b.line(points,width*.30,C.white,alpha*.65)
    end
    function b.finish() return table.concat(p)..op(0) end
    return b
end
local function hash(v) local x=math.sin(v*91.733+17.133)*43758.5453;return x-math.floor(x) end
local function project(x,z) return {184+420*x/z,187+624/z} end
local function polar(r,a) return project(r*math.cos(a),8+r*math.sin(a)) end
local function arc(r,a,z,n)
    local p={};for i=0,n do p[#p+1]=polar(r,a+(z-a)*i/n) end;return p
end
-- Scene lights are analytic Lua geometry; other components use their
-- separately reviewed contour paths. No alternate hand/word redesign lives here.
function M.install(display,component)
    assert(component=='arena','only the analytic arena renderer is supported')
    assert(display.draw_vector_data,'procedural backend unavailable')
    display.procedural_lights=true
    local draw=display.draw_vector_data
    -- Geometry is generated once. Lua supplies the original per-lamp envelopes.
    local walls,arena={},{}
    for _,side in ipairs({-1,1}) do for i=0,35 do
        local key=i*109+side*31;local depth=math.min(.995,(i%12+hash(key+7)*.88)/12)
        local perspective=.25+depth*.75;local x=184+side*(6+depth*181)
        local top,bottom=164-depth*124,170+depth*44;local y=top+(bottom-top)*hash(key+13)
        local size=(3.1+perspective*7.1)*(.58+hash(key+43)*.86)
        walls[#walls+1]={p={{x,y-size*.5},{x,y+size*.5}},width=.38+perspective*.78,
            color=hash(key+59)>.84 and C.orange or hash(key+61)>.62 and C.purple or C.cyan}
    end end
    arena[1]={base=true}
    for ring,r in ipairs({.36,.68,1.05,1.45,1.87,2.28,2.7}) do
        local k=ring-1;local angle=TAU/(16+k*2)
        for j=0,15+k*2 do if hash(k*401+j*97)>=.45 then
            local start=(j+hash(k*181+j*43)*.52)*angle;local depth=(1-math.sin(start+angle*.5))*.5
            local stop=start+angle*(.44+hash(k*307+j*67)*1.05)*(.5+depth*1.18)
            arena[#arena+1]={p=arc(r,start,stop,5),width=.42+depth*2.25,gain=(.16+.84*depth)*.8,color=(k*3+j)%7<2 and C.purple or C.cyan}
        end end
    end
    for i=0,31 do if hash(i*173)>=.42 then
        local a=i/32*TAU;local inner=.3+hash(i*29)*1.72;local depth=(1-math.sin(a))*.5
        arena[#arena+1]={p={polar(inner,a),polar(math.min(2.62,inner+(.15+hash(i*47)*.42)*(.52+depth*1.2)),a)},
            width=.4+depth*2.15,gain=(.16+.84*depth)*.8,color=i%6<2 and C.purple or C.cyan}
    end end
    local seed=0x51D0E1;local function rand() seed=(seed*1664525+1013904223)&0xffffffff;return seed/4294967296 end
    for _=1,104*6+34*5 do rand() end
    for i=0,11 do
        local a=rand()*TAU;local r=.65+rand()*1.9;local len=.16+rand()*.42;local width=.035+rand()*.065
        rand();rand();local angle=a+(rand()>.72 and math.pi*.5 or 0)
        local x,z=math.cos(a)*r,8+math.sin(a)*r
        local ux,uz=math.cos(angle)*len*.5,math.sin(angle)*len*.5;local vx,vz=-math.sin(angle)*width*.5,math.cos(angle)*width*.5
        arena[#arena+1]={rect=true,p={project(x-ux-vx,z-uz-vz),project(x+ux-vx,z+uz-vz),project(x+ux+vx,z+uz+vz),project(x-ux+vx,z-uz+vz)},
            color=i%4==0 and C.purple or C.cyan,gain=(.42+(1-math.sin(a))*.5*.55)*.8}
    end
    display.draw_light_atlas=function(name,gains,transform)
        local list=name:find('wall') and walls or arena;assert(#gains==#list,'procedural light order mismatch')
        local g=builder();if list==arena then g.shape(arc(2.7,0,TAU,96),0x000811) end
        for i,v in ipairs(list) do
            local alpha=gains[i]*(v.gain or 1)
            if not v.base and alpha>.001 then
                if v.rect then g.shape(v.p,nil,v.color,1,alpha*.25);g.shape(v.p,v.color,nil,1,alpha)
                else g.neon(v.p,v.width,v.color,alpha) end
            end
        end
        local s,tx,ty=1,0,0;if transform then s,tx,ty=table.unpack(transform) end
        return draw(g.finish(),s,0,0,s,tx,ty,1)
    end
    print('qi-duel: Lua arena geometry active')
end
return M
