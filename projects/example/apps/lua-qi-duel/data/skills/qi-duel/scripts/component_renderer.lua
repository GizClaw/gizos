-- Scalable contour keyframes. Layout, timing and interpolation are supplied
-- by main.lua, exactly as for the original component calls.
local M={}
local groups={
 ['hud-states']='hud',['carousel-base']='carousel',['charge-cells']='carousel',
 countdown='countdown',['impact-labels']='impact',
 ['result-streaks']='settlement',['result-words']='settlement',
 ['action-hands']='actions',['action-opponent']='actions',
 ['beam-clash-h106']='clash',['beam-clash-amoled']='clash',
 ['beam-clash-fade-h106']='clash',['beam-clash-fade-amoled']='clash',
}
function M.install(display,component)
 local paths=require('component_paths')
 local affine,atlas=display.draw_affine_asset,display.draw_sprite_atlas
 local function selected(name)
  local key=name:match('^@qi%-duel/(.-)%.h2r[8s]$')
  return key and paths[key] and (component=='all' or groups[key]==component) and key
 end
 local function draw(entry,lo,hi,mix,a,b,c,d,e,f,opacity,cache,tile)
  if mix==1 then lo=hi;mix=0 elseif lo==hi then mix=0 end
  local first=assert(entry.frames[lo],'invalid vector component frame')
  local second=assert(entry.frames[hi],'invalid vector component blend frame')
  display.draw_vector_slice('@qi-duel/vector/components.h2vp',first[1],first[2],
    a,b,c,d,e,f,opacity or 1,second[1],second[2],mix,cache,tile)
 end
 display.draw_affine_asset=function(name,a,b,c,d,e,f,crop,opacity)
  local key=selected(name)
  if not key then return affine(name,a,b,c,d,e,f,crop,opacity) end
  local entry=paths[key];local row=crop and crop[2] or 0
  assert(not crop or (crop[1]==0 and crop[3]==entry.w and crop[4]==entry.h),'unsupported vector crop')
  assert(row%entry.h==0,'misaligned vector component frame')
  local frame=row//entry.h+1
  draw(entry,frame,frame,0,a,b,c,d,e+c*row,f+d*row,opacity,opacity==nil or opacity==1,true)
 end
 display.draw_sprite_atlas=function(name,lo,hi,mix,x,y,scale,opacity)
  local key=selected(name)
  if not key then return atlas(name,lo,hi,mix,x,y,scale,opacity) end
  scale=scale or 1
  local cache=key=='result-words' and (mix==0 or mix==1)
  draw(paths[key],lo,hi,mix,scale,0,0,scale,x,y,opacity,cache,key~='action-hands')
 end
end
return M
