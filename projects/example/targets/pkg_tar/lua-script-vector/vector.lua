local display = require('display')
local delay = require('delay')

display.clear('black')
display.fill_polygon({{10,10},{60,10},{60,40},{10,40}}, 'red')
display.fill_ellipse(100,100,20,10,'blue',0,90,110)

local source = {{0,10,10,10,10,'red'}, {1,-100000,40,100000,40,'blue'}}
local commands = display.compile_commands(source)
source[1][2] = 1000
source = nil
collectgarbage('collect')
display.draw_commands(commands,150,181,0,140,2,1,{r=0,g=255,b=0})
assert(not pcall(display.draw_commands,commands,0,240,0,0,0))
assert(not pcall(display.fill_polygon,{{0,0},{10,0},{0/0,10}},'white'))
assert(not pcall(display.fill_ellipse,10,10,1,0,'white'))
display.draw_commands(display.compile_commands({}))
local native = require('vector_native')
local mesh = native.new()
display.draw_mesh(mesh,{cache=true})
display.draw_mesh(mesh,{cache=true})
-- Empty clipping still prepares a different transform; identity must replace it.
display.draw_mesh(mesh,{matrix={-1,0,0,1,200,0},left=0,right=0,cache=true})
display.draw_mesh(mesh,{matrix={1,0,0,1,0,0},grid=0,cache=true})
-- A coordinate-cache hit without span replay must still read owned vertices.
display.draw_mesh(mesh)
native.move(mesh,10,40)
assert(not pcall(native.move,mesh,math.huge,0))
collectgarbage('collect')
display.draw_mesh(mesh,{left=160,right=190,top=60,bottom=90,cache=true})
mesh = nil
collectgarbage('collect')
local points, widths = {{20.5,210.5},{60.5,210.5}}, {3}
assert(not display.stroke_path(points,widths,'red',0,0,display.height,true,true))
assert(display.stroke_path(points,widths,'red',0,0,display.height,true,true))
display.stroke_path({{90.5,210.5},{130.5,210.5}},{1},'blue',0,0,display.height,false,false,true)
points, widths = nil, nil
collectgarbage('collect')
local background = display.capture_region(0,0,display.width,display.height)
display.restore_background(background)
local submitted, rectangles = display.present({retained=true})
assert(submitted==display.width*display.height and rectangles==1)
display.fill_rect(200,200,8,8,'red')
display.restore_background(background)
assert(display.present()==0)
local region = display.capture_region(5,5,16,16,'black')
display.draw_region(region,210,210,0,display.height,'black')
submitted, rectangles = display.present()
assert(submitted==1024 and rectangles==1)
assert(display.present()==0)
display.release_background()
background, region = nil, nil
collectgarbage('collect')
print('LUA_VECTOR calls=PASS')
while true do delay.delay_ms(20) end
