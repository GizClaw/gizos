local sk,vm=require('skeleton2d'),require('vmath')
local geometry=require('geometry')
local d,clock,delay,touch=require('display'),require('system'),require('delay'),require('lcd_touch')
local white={color='white',font_size=7}
local dark={r=12,g=18,b=28}
local cyan={r=0,g=255,b=255}
local function label(x,y,s) d.draw_text(x,y,s,white) end
local colors={63488,2047,65504,2016,63519,65535}
local function resource(length,width,color)
 return {vertices={{-3,-width},{length+3,-width},{length+3,width},{-3,width},{-4,-width},{4,-width},{4,width},{-4,width}},primitives={{0,1,4,color},{0,5,4,color}}}
end
local function make_scene(kind,n,part_count)
 local bones,parts,tracks,resources={},{},{},{}
 n=n or (kind==1 and 3 or 12)
 for i=1,n do
  local parent=i==1 and 0 or i-1
  local x,y,angle=i==1 and 20 or (kind==1 and 34 or 12),i==1 and 100 or 0,0
  bones[i]={parent,x,y,angle,1,1}
  local amp=kind==1 and .9 or .28
  tracks[i]={bone=i,channel=3,linear=true,shortest=true,keys={{0,angle},{500000,angle+amp},{1000000,angle},{1500000,angle-amp},{2000000,angle}}}
 end
 part_count=part_count or n
 for i=1,part_count do parts[i]={(i-1)%n+1,(i-1)%6+1,i,1,0,0,0,1,1} end
 for i=1,6 do resources[i]=resource(kind==1 and 34 or 14,5,colors[i]) end
 local def=sk.compile{schema_version=1,bones=bones,parts=parts,clips={{duration_us=2000000,tracks=tracks}}}
 local actor=def:instance();local writer,mesh=sk.mesh(def,resources,{vertices=part_count*8,primitives=part_count*2})
 return {name=kind==1 and 'MECHANICAL ARM' or 'CHAIN',def=def,actor=actor,writer=writer,mesh=mesh,bones=bones,n=n,p=part_count,matrices=vm.buffer(n*6),items=vm.buffer(part_count*9),bounds=vm.buffer(part_count*5),state=vm.buffer(part_count*4),root={1,0,0,1,0,0}}
end
-- These fixtures are App data. The engine does not distinguish anatomy.
-- Box rows: x0,x1,y0,y1,z0,z1,front,back,side,top RGB565.
local fixtures={
 {name='HUMAN WALK',ground=156,root_y=108,bob_floor=152,bob_length=47,bob_angle=.52,
  body={{-8,8,-33,1,-14,14,64008,48135,55879,64512},{8,8.2,-28,-3,-1,1,65535,65535,65535,65535}},
  head_x=0,head_y=-33,head={{-7,8,-21,0,-8,8,64816,31331,64816,31331},{-7,8,-22,-17,-8,8,31331,31331,31331,31331},
   {8,12,-10,-6,-2,2,64816,64816,64816,64816},{8,8.2,-14,-11,-6,-3,0,0,0,0},{8,8.2,-14,-11,3,6,0,0,0,0}},
  -- x,y,z,upper,lower,phase,color,width,swing,bend base,sine,positive sine,foot x0,x1
  limbs={{-3,-27,14,18,17,0,33808,3,.48,-.45,-.15,0,-2,5},
   {-3,0,7,24,23,math.pi,11002,4,.52,0,0,.85,-4,12},
   {3,-27,-14,18,17,math.pi,64816,3,.48,-.45,-.15,0,-2,5},
   {3,0,-7,24,23,0,2047,4,.52,0,0,.85,-4,12}}},
 {name='DOG TROT',ground=155,root_y=105,bob_floor=142,bob_length=38,bob_angle=.4,
  body={{-33,34,-10,10,-10,10,56585,31331,48196,60780}},
  head_x=31,head_y=-8,head={{-8,10,-15,8,-10,10,56585,31331,48196,60780},{10,23,-3,6,-6,6,56585,48196,56585,60780},
   {-5,3,-24,-15,-10,-6,48196,31331,56585,60780},{-5,3,-24,-15,6,10,48196,31331,56585,60780},
   {10,10.2,-10,-7,-7,-4,0,0,0,0},{10,10.2,-10,-7,4,7,0,0,0,0},{23,23.2,-2,2,-2,2,0,0,0,0}},
  tail={x=-31,y=-4,length=27,width=3,color=56585},
  limbs={{-21,7,9,19,19,0,31331,4,.4,0,0,.85,-4,12},
   {23,6,9,20,18,math.pi,31331,4,.4,0,0,-.85,-4,12},
   {-21,9,-9,19,19,math.pi,56585,4,.4,0,0,.85,-4,12},
   {23,8,-9,20,18,0,56585,4,.4,0,0,-.85,-4,12}}}
}
local face_corners={{2,3,7,6},{1,5,8,4},{1,4,3,2},{5,6,7,8},{1,2,6,5},{4,8,7,3}}
local function walker(fixture)
 local bones,parts,tracks,resources,depths,vertices,faces={},{},{},{},{},{},{}
 local function box(bone,b)
  local x0,x1,y0,y1,z0,z1=table.unpack(b)
  local corners={{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
   {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}}
  for side,indices in ipairs(face_corners)do
   local color=side==1 and b[7] or side==2 and b[8] or side==5 and b[10] or b[9]
   local first=#vertices+1
   for _,index in ipairs(indices)do
    local v=corners[index];vertices[#vertices+1]={bone,v[1],v[2],v[3]}
   end
   faces[#faces+1]={first=first,color=color,id=#faces+1,depth=0,visible=false,bone=bone}
  end
 end
 local function add(parent,x,y,z,angle,shapes,color)
  local i=#bones+1;bones[i]={parent,x,y,angle,1,1};depths[i]=z
  resources[i]={vertices={},primitives={}}
  for _,b in ipairs(shapes)do
   box(i,b)
   local r=resources[i];local first=#r.vertices+1
   for _,v in ipairs({{b[1],b[3]},{b[2],b[3]},{b[2],b[4]},{b[1],b[4]}})do r.vertices[#r.vertices+1]=v end
   r.primitives[#r.primitives+1]={0,first,4,color or b[9]}
  end
  parts[i]={i,i,math.floor(-z*10)+i,1,0,0,0,1,1};return i
 end
 local function track(bone,channel,fn)
  local keys={};for k=0,16 do keys[k+1]={k*125000,fn(k/16*math.pi*2)} end
  keys[17][2]=keys[1][2]
  tracks[#tracks+1]={bone=bone,channel=channel,linear=true,shortest=false,keys=keys}
 end
 local function segment(length,width,color)
  return {{-3,length+3,-width,width,-width,width,color,color,color,color}}
 end
 add(0,120,fixture.root_y,0,0,fixture.body)
 add(1,fixture.head_x,fixture.head_y,0,0,fixture.head)
 if fixture.tail then
  local t=fixture.tail
  local bone=add(1,t.x,t.y,0,math.pi,segment(t.length,t.width,t.color))
  track(bone,3,function(phase)return math.pi+.25+.18*math.sin(phase) end)
 end
 local limb_ids={}
 for _,v in ipairs(fixture.limbs)do
  local x,y,z,l1,l2,phase,color,width=table.unpack(v)
  local upper=add(1,x,y,z,math.pi/2,segment(l1,width,color))
  local lower=add(upper,l1,0,z,0,segment(l2,width,color))
  local foot_color=v[14]==12 and (fixture.tail and color or 65535) or 64816
  local tip=add(lower,l2,0,z,-math.pi/2,{{v[13],v[14],-3,4,-width,width,foot_color,foot_color,foot_color,foot_color}})
  local function swing(t)return v[9]*math.cos(t+phase) end
  local function bend(t)local s=math.sin(t+phase);return v[10]+v[11]*s+v[12]*math.max(0,s) end
  track(upper,3,function(t)return math.pi/2+swing(t) end)
  track(lower,3,bend)
  track(tip,3,function(t)return -math.pi/2-swing(t)-bend(t) end)
  limb_ids[#limb_ids+1]={upper,lower,tip,v}
 end
 track(1,2,function(t)return fixture.bob_floor-fixture.bob_length*math.cos(fixture.bob_angle*math.cos(t)) end)
 local n=#bones;local vc,pc=0,0
 for _,r in ipairs(resources)do vc=vc+#r.vertices;pc=pc+#r.primitives end
 local def=sk.compile{schema_version=1,bones=bones,parts=parts,clips={{duration_us=2000000,tracks=tracks}}}
 local writer,mesh=sk.mesh(def,resources,{vertices=vc,primitives=pc})
 local spatial_writer,spatial_mesh=geometry.mesh(#vertices,#faces)
 local face_scratch={};for i,f in ipairs(faces)do face_scratch[i]=f end
 return {name=fixture.name,def=def,actor=def:instance(),writer=writer,mesh=mesh,bones=bones,parts=parts,n=n,p=n,
  matrices=vm.buffer(n*6),items=vm.buffer(n*9),bounds=vm.buffer(n*5),state=vm.buffer(n*4),root={1,0,0,1,0,0},ground=fixture.ground,
  depths=depths,vertices=vertices,faces=faces,face_scratch=face_scratch,xyz=vm.buffer((#vertices+n+2)*3),camera_xyz=vm.buffer((#vertices+n+2)*3),
  xy=vm.buffer(#vertices*2),topology=vm.buffer(#faces*4),camera_values={1,0,0,0,0,1,0,0,0,0,1,0},camera_matrix=vm.buffer(12),
  spatial_writer=spatial_writer,spatial_mesh=spatial_mesh,draw_order=vm.buffer(n),limb_ids=limb_ids,fixture=fixture}
end
local scenes={make_scene(1),walker(fixtures[1]),make_scene(3),walker(fixtures[2])}

local scene_id,time_us,playing,overlay,grid,mix,speed,flip=1,0,false,false,0,0,1,false
local mirrored,rotation,pitch,zoom_id,pan_id=false,0,0,1,1
local zooms={1,1.25,1.5,.75}
local pans={{0,0},{20,0},{0,15},{-20,0},{0,-15}}
local function apply_view(scene)
 local z=zooms[zoom_id];local sign=mirrored and -1 or 1;local m=scene.root
 if scene.vertices then
  m[1],m[2],m[3],m[4],m[5],m[6]=1,0,0,1,0,0
 else
  m[1],m[2],m[3],m[4]=z*sign,0,0,z
  m[5],m[6]=120+pans[pan_id][1]-120*m[1],105+pans[pan_id][2]-105*z
 end
end
local function evaluate(scene,t,weight,reverse)
 if weight==0 then scene.actor:sample(1,t,'repeat','current')
 else scene.actor:sample(1,t,'repeat','a');scene.actor:sample(1,t+500000,'repeat','b');scene.actor:blend(weight) end
 for i=1,scene.p do
  local at=(i-1)*4;scene.state:set(at+1,i);scene.state:set(at+2,scene.parts and scene.parts[i][2] or (i-1)%6+1)
  local layer=scene.parts and scene.parts[i][3] or i;scene.state:set(at+3,reverse and -layer or layer);scene.state:set(at+4,1)
 end
 scene.actor:set_parts(scene.state);scene.actor:evaluate(scene.root)
end
local function face_before(a,b)
 if a.depth==b.depth then return a.id<b.id end
 return a.depth<b.depth
end
-- Lua's table.sort invokes a non-yieldable C callback. Keep sorting in Lua
-- so the Host instruction hook can yield during larger fixture updates.
local function sort_faces(scene)
 local src,dst=scene.faces,scene.face_scratch;local n=#src;local width=1
 while width<n do
  for first=1,n,width*2 do
   local mid=math.min(first+width,n+1);local last=math.min(first+width*2,n+1)
   local left,right=first,mid
   for out=first,last-1 do
    if right>=last or (left<mid and face_before(src[left],src[right])) then
     dst[out]=src[left];left=left+1
    else dst[out]=src[right];right=right+1 end
   end
  end
  src,dst=dst,src;width=width*2
 end
 scene.faces,scene.face_scratch=src,dst
end
local function update_spatial(scene)
 scene.actor:copy_matrices(scene.matrices)
 local matrices,xyz=scene.matrices,scene.xyz
 for i,v in ipairs(scene.vertices)do
  local at=(v[1]-1)*6;local out=(i-1)*3
  xyz:set(out+1,matrices:get(at+1)*v[2]+matrices:get(at+3)*v[3]+matrices:get(at+5))
  xyz:set(out+2,matrices:get(at+2)*v[2]+matrices:get(at+4)*v[3]+matrices:get(at+6))
  xyz:set(out+3,scene.depths[v[1]]+v[4])
 end
 local nv=#scene.vertices
 for i=1,scene.n do
  local at=(nv+i-1)*3
  xyz:set(at+1,matrices:get((i-1)*6+5));xyz:set(at+2,matrices:get((i-1)*6+6));xyz:set(at+3,scene.depths[i])
 end
 for i=1,2 do
  local at=(nv+scene.n+i-1)*3
  xyz:set(at+1,i==1 and 12 or 228);xyz:set(at+2,scene.ground);xyz:set(at+3,0)
 end
 -- Row-major camera matrix: +X forward, +Y down, +Z character width.
 -- Camera depth grows toward the viewer. A downward view exposes top faces.
 local c,s=math.cos(rotation*math.pi/180),math.sin(rotation*math.pi/180)
 local cp,sp=math.cos(pitch*math.pi/180),math.sin(pitch*math.pi/180)
 local z=zooms[zoom_id];local sign=mirrored and -1 or 1;local m=scene.camera_values
 m[1],m[2],m[3]=c*z*sign,0,s*z*sign
 m[4]=120+pans[pan_id][1]-120*m[1]
 m[5],m[6],m[7]=s*sp*z,cp*z,-c*sp*z
 m[8]=117+pans[pan_id][2]-120*m[5]-110*m[6]
 m[9],m[10],m[11],m[12]=s*cp,-sp,-c*cp,-120*s*cp+110*sp
 scene.camera_matrix:load(m)
 geometry.affine3(scene.camera_xyz,xyz,scene.camera_matrix,nv+scene.n+2)
 local points=scene.camera_xyz
 for i=1,nv do scene.xy:set(i*2-1,points:get(i*3-2));scene.xy:set(i*2,points:get(i*3-1)) end
 for _,f in ipairs(scene.faces)do
  local at=(f.first-1)*3;local ax,ay=points:get(at+1),points:get(at+2)
  local bx,by=points:get(at+4),points:get(at+5);local cx,cy=points:get(at+7),points:get(at+8)
  f.visible=((bx-ax)*(cy-ay)-(by-ay)*(cx-ax))*sign < -1e-7
  f.depth=(points:get(at+3)+points:get(at+6)+points:get(at+9)+points:get(at+12))/4
 end
 sort_faces(scene)
 local count=0
 scene.bounds:fill(0);scene.draw_order:fill(0)
 for j=1,#scene.faces do
  local f=scene.faces[flip and #scene.faces-j+1 or j]
  if f.visible then
   local at=count*4;scene.topology:set(at+1,0);scene.topology:set(at+2,f.first);scene.topology:set(at+3,4);scene.topology:set(at+4,f.color);count=count+1
   scene.draw_order:set(f.bone,count)
   if overlay then
    local base=(f.bone-1)*5
    for v=f.first,f.first+3 do
     local x,y=scene.xy:get(v*2-1),scene.xy:get(v*2)
     if scene.bounds:get(base+1)==0 then
      scene.bounds:set(base+1,1);scene.bounds:set(base+2,x);scene.bounds:set(base+3,y);scene.bounds:set(base+4,x);scene.bounds:set(base+5,y)
     else
      scene.bounds:set(base+2,math.min(x,scene.bounds:get(base+2)));scene.bounds:set(base+3,math.min(y,scene.bounds:get(base+3)))
      scene.bounds:set(base+4,math.max(x,scene.bounds:get(base+4)));scene.bounds:set(base+5,math.max(y,scene.bounds:get(base+5)))
     end
    end
   end
  end
 end
 geometry.update_mesh(scene.spatial_writer,scene.xy,scene.topology,nv,count)
 scene.visible_faces=count
end
local options={grid=0}
local function draw_scene(scene)
 if scene.vertices then
  update_spatial(scene);d.draw_mesh(scene.spatial_mesh,options)
 else
  sk.update_mesh(scene.writer,scene.actor);d.draw_mesh(scene.mesh,options)
 end
end
local function button(x,y,text,active)
 d.fill_rect(x+1,y,46,18,active and {r=30,g=85,b=105} or {r=30,g=40,b=55})
 label(x+math.floor((48-#text*6)/2),y+5,text)
end
local function draw()
 local scene=scenes[scene_id];apply_view(scene);evaluate(scene,time_us,mix,flip);options.grid=grid
 d.clear(dark)
 draw_scene(scene)
 if scene.vertices then
  local points=scene.camera_xyz;local at=(#scene.vertices+scene.n)*3
  d.draw_line(points:get(at+1),points:get(at+2),points:get(at+4),points:get(at+5),{r=45,g=55,b=65})
 end
 if overlay then
  if scene.vertices then
   local points=scene.camera_xyz
   for i=1,scene.n do
    local at=(#scene.vertices+i-1)*3;local x,y=points:get(at+1),points:get(at+2)
    d.fill_circle(x,y,2,'white');local parent=scene.bones[i][1]
    if parent>0 then local p=(#scene.vertices+parent-1)*3;d.draw_line(x,y,points:get(p+1),points:get(p+2),'white') end
    label(x,y,i..':'..scene.draw_order:get(i))
   end
  else
   scene.actor:copy_matrices(scene.matrices);scene.writer:copy_bounds(scene.bounds);scene.actor:copy_draw_items(scene.items)
   for i=1,scene.n do
    local x,y=scene.matrices:get((i-1)*6+5),scene.matrices:get((i-1)*6+6)
    d.fill_circle(x,y,2,'white');local parent=scene.bones[i][1]
    if parent>0 then d.draw_line(x,y,scene.matrices:get((parent-1)*6+5),scene.matrices:get((parent-1)*6+6),'white') end
   end
   for i=1,scene.p do local a=(i-1)*9;label(scene.items:get(a+8),scene.items:get(a+9),tostring(i)) end
  end
  for i=1,scene.p do local a=(i-1)*5
   if scene.bounds:get(a+1)==1 then
    local x,y,x2,y2=scene.bounds:get(a+2),scene.bounds:get(a+3),scene.bounds:get(a+4),scene.bounds:get(a+5)
    d.draw_line(x,y,x2,y,'white');d.draw_line(x2,y,x2,y2,'white');d.draw_line(x2,y2,x,y2,'white');d.draw_line(x,y2,x,y,'white')
   end
  end
 end
 d.fill_rect(0,0,240,55,dark)
 local facing=rotation==0 and 'RIGHT' or rotation==90 and 'FRONT' or rotation==180 and 'LEFT' or rotation==270 and 'BACK' or 'THREE QTR'
 label(6,4,string.format('%d DEG %s',rotation,facing))
 label(6,17,scene.name..'  '..scene.n..' BONES')
 button(0,35,'MIRROR',mirrored);button(48,35,'ROTATE',rotation~=0);button(96,35,'ZOOM',zoom_id~=1);button(144,35,'PAN',pan_id~=1);button(192,35,'RESET')
 button(0,55,'CAMERA',pitch~=0);button(192,55,'CHECK');label(55,60,string.format('PITCH %d',pitch))
 d.fill_rect(0,177,240,63,dark)
 d.fill_rect(8,182,224,3,'white');d.fill_rect(8+math.floor((time_us%2000000)/2000000*222),178,3,11,cyan)
 label(8,192,string.format('%dMS MIX%d %s',time_us%2000000//1000,math.floor(mix*100+.5),speed==.5 and 'HALF SPEED' or speed==2 and '2X SPEED' or '1X SPEED'))
 button(0,202,playing and 'PAUSE' or 'PLAY',playing)
 button(48,202,'PREV');button(96,202,'NEXT');button(144,202,'SCENE');button(192,202,'DEBUG',overlay)
 button(0,222,'MIX',mix>0);button(48,222,'SPEED',speed~=1);button(96,222,'ORDER',flip);button(144,222,'GRID'..grid,grid>0);button(192,222,'BENCH')
 d.present()
end
local function percentile(values,q) table.sort(values);return values[math.ceil(#values*q)] end
local function benchmark_spatial()
 local saved_rotation,saved_pitch,saved_overlay,saved_flip=rotation,pitch,overlay,flip
 local saved_zoom,saved_pan,saved_mirror=zoom_id,pan_id,mirrored
 overlay,flip,zoom_id,pan_id,mirrored=false,false,1,1,false
 for _,scene in ipairs({scenes[2],scenes[4]})do
  for _,view in ipairs({{0,0},{45,0},{0,45},{45,45}})do
   rotation,pitch=view[1],view[2]
   for run=1,3 do
    local calc,geom,raster,present,total,switch={},{},{},{},{},{}
    local pixels,rects=0,0
    for frame=-299,3000 do
     local a=clock.micros();apply_view(scene);evaluate(scene,frame*33333,.5,false)
     local b=clock.micros();update_spatial(scene);local c=clock.micros()
     d.clear('black');d.draw_mesh(scene.spatial_mesh);local e=clock.micros()
     local px,rc=d.present();local f=clock.micros()
     if frame>0 then
      calc[frame]=b-a;geom[frame]=c-b;raster[frame]=e-c;present[frame]=f-e;total[frame]=f-a
      pixels=pixels+px;rects=rects+rc
     end
     if frame%30==0 then delay.delay_ms(1) end
    end
    -- View switching is measured independently of raster and retains the pose.
    for i=1,300 do
     rotation=(rotation+45)%360;pitch=45-pitch
     local a=clock.micros();update_spatial(scene);switch[i]=clock.micros()-a
    end
    rotation,pitch=view[1],view[2]
    local slow=0;for _,v in ipairs(total)do if v>33333 then slow=slow+1 end end
    print(string.format('SKELETON SPATIAL name=%s yaw=%d pitch=%d run=%d bones=%d vertices=%d faces=%d vm_kib=%.1f',scene.name,rotation,pitch,run,scene.n,#scene.vertices,#scene.faces,collectgarbage('count')))
    print(string.format('SKELETON SPATIAL calc_p95_us=%.1f geometry_p95_us=%.1f raster_p95_us=%.1f present_p95_us=%.1f switch_p95_us=%.1f',percentile(calc,.95),percentile(geom,.95),percentile(raster,.95),percentile(present,.95),percentile(switch,.95)))
    print(string.format('SKELETON SPATIAL total_p50_us=%.1f total_p95_us=%.1f total_p99_us=%.1f max_us=%.1f slow=%d pixels_mean=%.1f rects_mean=%.1f',percentile(total,.5),percentile(total,.95),percentile(total,.99),total[#total],slow,pixels/3000,rects/3000))
    delay.delay_ms(1)
   end
  end
 end
 rotation,pitch,overlay,flip=saved_rotation,saved_pitch,saved_overlay,saved_flip
 zoom_id,pan_id,mirrored=saved_zoom,saved_pan,saved_mirror
 print('SKELETON SPATIAL COMPLETE')
end
local function benchmark()
 playing=false
 local ok=clock.micros();if not ok then print('SKELETON BENCH SKIP no microsecond clock');return end
 local overhead={};for i=1,1000 do local a=clock.micros();overhead[i]=clock.micros()-a end
 print('SKELETON BENCH clock_overhead_p50_us='..percentile(overhead,.5))
 -- tier, active viewport edge, animated, blend, changing order
 local workloads={{1,240,true,.5,true},{2,240,true,.5,true},{3,240,true,.5,true},
  {1,120,true,.5,true},{1,240,false,0,false},{1,120,false,0,false},
  {1,240,true,0,false},{1,240,true,.5,false}}
 for case_id,workload in ipairs(workloads)do
  local tier,edge,animated,weight,changing=table.unpack(workload)
  local clip={left=0,top=0,right=edge,bottom=edge,cache=false}
  local nb=16 << (tier-1);local np=nb*3//2;local actors={}
  for i=1,1 << (tier-1) do actors[i]=make_scene(3,nb,np);actors[i].root[1]=.3;actors[i].root[4]=.3;actors[i].root[5]=(i-1)*42;actors[i].root[6]=60+(i%2)*50 end
  collectgarbage('collect');local base_kib=collectgarbage('count');d.clear('black');d.present()
  for run=1,3 do
   local calc,geom,raster,present,total,recovery={},{},{},{},{},{};for i=1,3000 do calc[i]=0;geom[i]=0;raster[i]=0;present[i]=0;total[i]=0;recovery[i]=0 end
   local pixels,rects=0,0
   for frame=-299,3000 do
    local a=clock.micros()
    for _,scene in ipairs(actors) do evaluate(scene,animated and frame*33333 or 0,weight,changing and frame%2==0) end
    local b=clock.micros()
    for _,scene in ipairs(actors) do sk.update_mesh(scene.writer,scene.actor) end
    local c=clock.micros();d.fill_rect(0,0,edge,edge,'black');local recovered=clock.micros();for _,scene in ipairs(actors) do d.draw_mesh(scene.mesh,clip) end
    local e=clock.micros();local px,rc=d.present();local f=clock.micros()
    if frame>0 then recovery[frame]=recovered-c;calc[frame]=b-a;geom[frame]=c-b;raster[frame]=e-c;present[frame]=f-e;total[frame]=f-a;pixels=pixels+px;rects=rects+rc end
    if frame%30==0 then delay.delay_ms(1) end
   end
   local slow=0;for i=1,#total do if total[i]>33333 then slow=slow+1 end end
   local prefix=string.format('SKELETON BENCH tier=%d run=%d case=%d ',tier,run,case_id)
   print(prefix..string.format('viewport_percent=%d animated=%s blend=%.1f changing_order=%s recovery_p95_us=%.1f',edge==120 and 25 or 100,tostring(animated),weight,tostring(changing),percentile(recovery,.95)))
   print(prefix..string.format('bones=%d parts=%d vertices=%d actors=%d vm_kib=%.1f definition_bytes=%d actor_bytes=%d writer_bytes=%d',nb,np,np*8,#actors,base_kib,actors[1].def:bytes(),actors[1].actor:bytes(),actors[1].writer:bytes()))
   print(prefix..string.format('calc_p95_us=%.1f geometry_p95_us=%.1f raster_p95_us=%.1f present_p95_us=%.1f',percentile(calc,.95),percentile(geom,.95),percentile(raster,.95),percentile(present,.95)))
   print(prefix..string.format('total_p50_us=%.1f total_p95_us=%.1f total_p99_us=%.1f max_us=%.1f slow=%d pixels_mean=%.1f rects_mean=%.1f',percentile(total,.5),percentile(total,.95),percentile(total,.99),total[#total],slow,pixels/3000,rects/3000))
   delay.delay_ms(1)
  end
 end
 benchmark_spatial();print('SKELETON BENCH COMPLETE');draw()
end
-- Optional fixture validation uses closed-form joint landmarks as an oracle,
-- independently of the production hierarchy and camera matrix construction.
local function validate_walkers()
 local saved_scene,saved_time,saved_rotation,saved_pitch=scene_id,time_us,rotation,pitch
 local saved_mix,saved_flip,saved_mirror,saved_zoom,saved_pan=mix,flip,mirrored,zoom_id,pan_id
 mix,flip,mirrored,zoom_id,pan_id=0,false,false,1,1
 local cases=0
 local function close(actual,expected)assert(math.abs(actual-expected)<1e-7,'fixture landmark mismatch') end
 for _,id in ipairs({2,4})do
  scene_id=id;local scene=scenes[id];local fixture=scene.fixture
  local definition,actor=scene.def,scene.actor
  for _,t in ipairs({0,250000,500000,1000000,1500000,2000000})do
   time_us=t;local phase=t/2000000*math.pi*2
   local root_y=fixture.bob_floor-fixture.bob_length*math.cos(fixture.bob_angle*math.cos(phase))
   for yaw=0,315,45 do for tilt=0,45,45 do
    rotation,pitch=yaw,tilt;apply_view(scene);evaluate(scene,t,0,false)
    sk.update_mesh(scene.writer,scene.actor);update_spatial(scene)
    assert(scene.def==definition and scene.actor==actor,'view rebuilt actor')
    local function joint(bone,x,y,z)
     close(scene.matrices:get((bone-1)*6+5),x);close(scene.matrices:get((bone-1)*6+6),y)
     local c,s=math.cos(yaw*math.pi/180),math.sin(yaw*math.pi/180)
     local cp,sp=math.cos(tilt*math.pi/180),math.sin(tilt*math.pi/180)
     local depth=(x-120)*s-z*c;local at=(#scene.vertices+bone-1)*3
     close(scene.camera_xyz:get(at+1),120+(x-120)*c+z*s)
     close(scene.camera_xyz:get(at+2),117+(y-110)*cp+depth*sp)
     close(scene.camera_xyz:get(at+3),depth*cp-(y-110)*sp)
    end
    joint(1,120,root_y,0)
    joint(2,120+fixture.head_x,root_y+fixture.head_y,0)
    for _,limb in ipairs(scene.limb_ids)do
     local upper,lower,tip,v=table.unpack(limb);local p=phase+v[6]
     local a=math.pi/2+v[9]*math.cos(p)
     local bend=v[10]+v[11]*math.sin(p)+v[12]*math.max(0,math.sin(p))
     local hx,hy=120+v[1],root_y+v[2]
     local kx,ky=hx+v[4]*math.cos(a),hy+v[4]*math.sin(a)
     joint(upper,hx,hy,v[3]);joint(lower,kx,ky,v[3])
     joint(tip,kx+v[5]*math.cos(a+bend),ky+v[5]*math.sin(a+bend),v[3])
    end
    cases=cases+1
   end end
   delay.delay_ms(1)
  end
 end
 scene_id,time_us,rotation,pitch=saved_scene,saved_time,saved_rotation,saved_pitch
 mix,flip,mirrored,zoom_id,pan_id=saved_mix,saved_flip,saved_mirror,saved_zoom,saved_pan
 print('SKELETON LANDMARKS PASS cases='..cases);draw()
end
local input_step=0
local last=clock.millis()
draw();print('SKELETON FRAME initial')
while true do
 local ev=touch.poll();local changed=false
 if ev.just_pressed then
  input_step=input_step+1
  local x,y=ev.x,ev.y
  if y>=35 and y<55 then
   if x<48 then mirrored=not mirrored
   elseif x<96 then
    if scene_id~=2 and scene_id~=4 then scene_id=2 end
    rotation=(rotation+45)%360
   elseif x<144 then zoom_id=zoom_id%#zooms+1
   elseif x<192 then pan_id=pan_id%#pans+1
   else mirrored,rotation,pitch,zoom_id,pan_id=false,0,0,1,1 end
  elseif y>=55 and y<73 then
   if x>=192 then validate_walkers()
   elseif x<48 then
    if scene_id~=2 and scene_id~=4 then scene_id=2 end
    pitch=pitch==0 and 45 or 0
   end
  elseif y>=177 and y<191 then time_us=math.floor(math.max(0,math.min(1,(x-8)/224))*2000000);playing=false
  elseif y>=202 and y<222 then
   if x<48 then playing=not playing
   elseif x<96 then time_us=math.floor(math.floor((time_us/1e6*30-1)+.5)*1000000/30+.5);playing=false
   elseif x<144 then time_us=math.floor(math.floor((time_us/1e6*30+1)+.5)*1000000/30+.5);playing=false
   elseif x<192 then scene_id=scene_id%#scenes+1;time_us=0
   else overlay=not overlay end
  elseif y>=222 then
   if x<48 then mix=(math.floor(mix*10+.5)+2)%12/10
   elseif x<96 then speed=speed==1 and .5 or speed==.5 and 2 or 1
   elseif x<144 then flip=not flip
   elseif x<192 then grid=(grid+1)%17
   else benchmark() end
  end
  changed=true
 end
 local now=clock.millis();if playing then time_us=time_us+math.floor((now-last)*1000*speed);changed=true end;last=now
 if changed then draw() end
 if ev.just_pressed then print(string.format('SKELETON INPUT scene=%d time=%d playing=%s overlay=%s grid=%d order=%s mirror=%s angle=%d zoom=%d pan=%d pitch=%d step=%d; speed=%g mix=%.1f',scene_id,time_us,tostring(playing),tostring(overlay),grid,tostring(flip),tostring(mirrored),rotation,math.floor(zooms[zoom_id]*100),pan_id,pitch,input_step,speed,mix)) end
 delay.delay_ms(16)
end
