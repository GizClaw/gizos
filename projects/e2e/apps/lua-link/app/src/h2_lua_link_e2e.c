#include "h2_lua_link_e2e.h"

#include "h2_lua.h"
#include "h2_lua_job.h"
#include "h2_lua_link.h"

#include <stdio.h>
#include <string.h>

/* Both boards run this script; `args.role` picks the side. Every result is a
 * `LINK stage=...` line on the Runtime Log so the two consoles can be
 * compared. */
static const char s_script[] =
    "local link=require('link');local rt=require('runtime');"
    "local sys=require('system');local ev=rt.event;"
    "local host=args.role=='host';local s={msgs={},dgrams={}};"
    "local function now() return sys.millis() end;"
    "link.on(ev.LINK_CONNECTED,function(e) s.role=e.role;s.maxd=e.max_datagram end);"
    "link.on(ev.LINK_MESSAGE,function(e) local q=e.reliable and s.msgs or s.dgrams;"
    "q[#q+1]=e.data end);"
    "link.on(ev.LINK_DISCONNECTED,function(e) s.disc=e.reason..':'..e.result end);"
    "link.on(ev.LINK_ERROR,function(e) s.err=e.reason..':'..e.result end);"
    "local function wait(f,ms) local t=now()+ms;while not f() do "
    "if s.err then error('link error '..s.err) end;"
    "if now()>t then error('timeout') end;rt.sleep(2) end end;"
    "local rpos=0;"
    "local function recv(ms) wait(function() return #s.msgs>rpos or s.disc end,ms);"
    "if #s.msgs<=rpos then error('disconnected '..s.disc) end;"
    "rpos=rpos+1;local m=s.msgs[rpos];s.msgs[rpos]=false;return m end;"
    "local function send(m) while true do local ok,err=link.send(m);"
    "if ok then return end;assert(err=='link: busy',err);rt.sleep(2) end end;"
    "local function write_all(d) local off=1;while off<=#d do "
    "local n=assert(link.write(off==1 and d or d:sub(off)));off=off+n;"
    "if n==0 then rt.sleep(2) end end end;"
    "local function pattern(n) local t={};for i=0,n-1 do t[#t+1]=string.char(i%251) end;"
    "return table.concat(t) end;"
    "local function stream_send(total) local b=pattern(1004);local sent=0;"
    "while sent<total do local k=math.min(#b,total-sent);write_all(b:sub(1,k));"
    "sent=sent+k end end;"
    "local function stream_recv(total) local got=0;local t=now();"
    "while got<total do local c,err=link.read(4096,5000);assert(c,err);"
    "if #c==0 then error('stream stalled at '..got) end;"
    "for i=1,#c do if c:byte(i)~=((got+i-1)%1004)%251 then "
    "error('stream corrupt at '..(got+i)) end end;got=got+#c end;"
    "return now()-t end;"
    "local t0=now();"
    /* Hold mode: stay connected at 10 Hz until the link drops, then report
     * how long after the last datagram the drop surfaced. */
    "if args.mode=='hold' then "
    "if host then assert(link.host({tag='gizos-lua-link-hold'})) "
    "else assert(link.join({tag='gizos-lua-link-hold',timeout_ms=60000})) end;"
    "wait(function() return s.role end,90000);"
    "print('LINK stage=hold_connected role='..s.role);"
    "local last=now();local seen=0;"
    "while not s.disc do link.send_unreliable('tick');"
    "if #s.dgrams>seen then seen=#s.dgrams;last=now() end;rt.sleep(100) end;"
    "print('LINK stage=hold_end reason='..s.disc..' ms_since_last_datagram='..(now()-last)..' ticks='..seen);"
    "return 'hold-ok' end;"
    "if host then assert(link.host({tag='gizos-lua-link-e2e'})) "
    "else assert(link.join({tag='gizos-lua-link-e2e',timeout_ms=60000})) end;"
    "print('LINK stage=start role='..args.role..' state='..link.state());"
    "wait(function() return s.role end,90000);"
    "print('LINK stage=connected role='..s.role..' max_datagram='..s.maxd..' ms='..(now()-t0));"
    /* 1. Round-trip time over reliable messages. */
    "local n=20;local mn,mx,sum=1e9,0,0;"
    "for i=1,n do if host then local t=now();send('ping'..i);"
    "assert(recv(5000)=='pong'..i);local d=now()-t;sum=sum+d;"
    "if d<mn then mn=d end;if d>mx then mx=d end "
    "else assert(recv(5000)=='ping'..i);send('pong'..i) end end;"
    "if host then print(string.format('LINK stage=rtt count=%d min_ms=%d avg_ms=%d max_ms=%d',"
    "n,mn,sum//n,mx)) end;"
    /* 2. Reliable burst each way: order and loss. */
    "local burst=200;local pad=string.rep('r',60);"
    "local function burst_send() local t=now();for i=1,burst do "
    "send(string.format('%04d',i)..pad) end;return now()-t end;"
    "local function burst_recv() local t=now();for i=1,burst do local m=recv(10000);"
    "assert(m:sub(1,4)==string.format('%04d',i),'order at '..i) end;return now()-t end;"
    "if host then local ts=burst_send();assert(recv(20000)=='burst-ok');"
    "local tr=burst_recv();send('burst-ok');"
    "print(string.format('LINK stage=reliable_burst count=%d bytes=64 send_ms=%d recv_ms=%d',burst,ts,tr)) "
    "else local tr=burst_recv();send('burst-ok');burst_send();assert(recv(20000)=='burst-ok');"
    "print(string.format('LINK stage=reliable_burst count=%d recv_ms=%d',burst,tr)) end;"
    /* 3. Datagrams at 50 Hz each way: count what arrives. */
    "local dn=100;local dpad=string.rep('d',96);"
    "local function dgram_send() local busy=0;for i=1,dn do "
    "local ok=link.send_unreliable(string.format('%04d',i)..dpad);"
    "if not ok then busy=busy+1 end;rt.sleep(20) end;return busy end;"
    "local function dgram_count() local c=0;for _,m in ipairs(s.dgrams) do "
    "if #m==100 then c=c+1 end end;s.dgrams={};return c end;"
    "if host then local busy=dgram_send();send('dgram-done');"
    "local peer=recv(10000);local b2=0;"
    "assert(recv(20000)=='dgram-done');rt.sleep(500);local got=dgram_count();"
    "print(string.format('LINK stage=datagram sent=%d busy=%d peer_%s recv=%d',dn,busy,peer,got));"
    "send('recv='..got) "
    "else assert(recv(20000)=='dgram-done');rt.sleep(500);local got=dgram_count();"
    "send('recv='..got);local busy=dgram_send();send('dgram-done');"
    "local peer=recv(10000);"
    "print(string.format('LINK stage=datagram recv=%d sent=%d busy=%d peer_%s',got,dn,busy,peer)) end;"
    /* 4. Byte stream each way. */
    "local total=65536;"
    "if host then local t=now();stream_send(total);local rep=recv(60000);"
    "local ms=recv(60000);local tr=stream_recv(total);send('stream-ok');"
    "print(string.format('LINK stage=stream to_join_%s join_ms=%s from_join_bytes=%d ms=%d kbps=%d',"
    "rep,ms,total,tr,total*8//math.max(tr,1))) "
    "else local tr=stream_recv(total);send('ok');send(tostring(tr));stream_send(total);"
    "assert(recv(60000)=='stream-ok');"
    "print(string.format('LINK stage=stream from_host_bytes=%d ms=%d kbps=%d',total,tr,total*8//math.max(tr,1))) end;"
    /* 5. The joiner leaves; the host must see peer_closed promptly. */
    "if host then local t=now();send('leave');"
    "wait(function() return s.disc end,10000);"
    "print('LINK stage=peer_exit reason='..s.disc..' ms='..(now()-t)..' state='..link.state()) "
    "else assert(recv(20000)=='leave');link.close() end;"
    "return 'link-ok'";

h2_pal_result_t h2_lua_link_e2e_run(h2_runtime_t *runtime,
                                    const h2_lua_link_e2e_config_t *config) {
  const h2_lua_host_config_t host_config = {
      .runtime = runtime,
      .worker_count = 1u,
      .max_jobs = 1u,
      .execution_timeout_ms = 300000u,
      .vm_memory_limit_bytes = 512u * 1024u,
      .output_limit_bytes = 8192u,
  };
  const h2_lua_link_config_t link_config = {
      .adv_type = config->adv_type,
      .scan_type = config->scan_type,
  };
  const h2_lua_arg_t args[] = {
      {"role", config->role},
      {"mode", config->hold ? "hold" : "suite"},
  };
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status = {0};
  h2_pal_result_t rc = h2_lua_host_create(&host_config, &host);
  if (rc == H2_PAL_OK) {
    rc = h2_lua_link_enable(host, &link_config);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_lua_host_start(host);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_lua_job_submit_text(host, "@lua_link_e2e.lua",
                                (const uint8_t *)s_script,
                                sizeof(s_script) - 1u, args, 2u, &job);
  }
  while (rc == H2_PAL_OK) {
    rc = h2_lua_job_get_status(host, job, &status);
    if (rc != H2_PAL_OK || (status.state != H2_LUA_JOB_QUEUED &&
                            status.state != H2_LUA_JOB_RUNNING &&
                            status.state != H2_LUA_JOB_WAITING)) {
      break;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 100u);
  }
  if (rc == H2_PAL_OK && status.state != H2_LUA_JOB_SUCCEEDED) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  {
    char line[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(line, sizeof(line),
                   "H2_LUA_LINK_E2E result=%s role=%s rc=%d state=%d "
                   "message=%s",
                   rc == H2_PAL_OK ? "PASS" : "FAIL", config->role, (int)rc,
                   (int)status.state, status.message);
    (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "lua-link-e2e",
                           line);
  }
  if (job != H2_LUA_JOB_ID_NONE) {
    (void)h2_lua_job_release(host, job);
  }
  h2_lua_host_destroy(host);
  return rc;
}
