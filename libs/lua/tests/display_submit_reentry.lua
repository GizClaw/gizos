local d,p,r=require('display'),require('submit_test'),require('runtime')
local function flush()
 local s,e=d.flush()
 while not s do assert(e==d.BUSY);r.sleep(1);s,e=d.flush() end
 return s
end
-- Both present and submit now allocate the worker baseline and snapshot on
-- first use. Force finalizer reentry during that first submission.
do
 collectgarbage('collect');collectgarbage('incremental')
 local pause=collectgarbage('param','pause',0)
 local mul=collectgarbage('param','stepmul',0)
 local step=collectgarbage('param','stepsize',0)
 collectgarbage('stop')
 local fired=false
 local garbage=setmetatable({}, {__gc=function()
  assert(p.in_call(d.submit),'must reenter during native submit allocation')
  fired=true;d.clear('red');assert(d.submit()==1);d.clear('blue')
 end})
 garbage=nil;collectgarbage('restart')
 local sequence,err=d.submit()
 assert(fired and sequence==nil and err<0,'outer preparation must yield: '..tostring(fired)..'/'..tostring(sequence)..'/'..tostring(err))
 collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
 collectgarbage('param','stepsize',step)
 local s=flush();assert(s.completed==1 and s.successful==1 and p.pixel()==63488)
 assert(d.submit()==2);s=flush();assert(s.completed==2 and p.pixel()==31)
end
-- Keep the common fixture's final image so native/browser checks stay comparable.
d.clear('black');d.fill_rect(0,0,32,32,'blue');d.fill_rect(32,0,32,32,'red');d.present()
print('LUA_SUBMIT allocation reentry=PASS')
