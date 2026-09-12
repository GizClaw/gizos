#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#include "h2_lua.h"
#include "h2_lua_capability.h"
#include "h2_lua_job.h"
#include "h2_lua_link.h"
#include "h2_lua_link_fake_ble.h"
#include "h2_pal.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int s_marks[2];

/* capability.call('mark', ...) lets a script tell the test it reached a
 * checkpoint without the test reading Lua state. */
static h2_pal_result_t mark_call(void *user, h2_lua_capability_request_id_t id,
                                 const char *input, const char *options,
                                 char *output, size_t output_capacity,
                                 const char **out_error) {
  (void)id;
  (void)input;
  (void)options;
  (void)out_error;
  atomic_fetch_add((atomic_int *)user, 1);
  (void)snprintf(output, output_capacity, "ok");
  return H2_PAL_OK;
}

static h2_lua_host_t *create_host(h2_runtime_t *runtime, int enable_link,
                                  atomic_int *mark) {
  const h2_lua_host_config_t config = {
      .runtime = runtime,
      .worker_count = 2u,
      .max_jobs = 2u,
      .execution_timeout_ms = 60000u,
  };
  const h2_lua_link_config_t link_config = {
      .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
  };
  h2_lua_host_t *host = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  if (enable_link) {
    assert(h2_lua_link_enable(host, &link_config) == H2_PAL_OK);
    assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_INVALID_STATE);
  }
  if (mark != NULL) {
    assert(h2_lua_register_capability(host, "mark", mark_call, NULL, mark) ==
           H2_PAL_OK);
  }
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  return host;
}

static h2_lua_job_id_t submit(h2_lua_host_t *host, const char *name,
                              const char *script, const char *tag) {
  const h2_lua_arg_t args[] = {{"tag", tag}};
  h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
  assert(h2_lua_job_submit_text(host, name, (const uint8_t *)script,
                                strlen(script), args, 1u,
                                &job) == H2_PAL_OK);
  return job;
}

static int is_terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static h2_lua_job_status_t wait_job(h2_lua_host_t *host, h2_lua_job_id_t job) {
  h2_lua_job_status_t status;
  for (int i = 0; i < 20000; ++i) {
    assert(h2_lua_job_get_status(host, job, &status) == H2_PAL_OK);
    if (is_terminal(status.state)) {
      return status;
    }
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  fprintf(stderr, "job %u stuck: state=%d message=%s\n", (unsigned)job,
          (int)status.state, status.message);
  assert(!"Lua job did not finish");
  return status;
}

static void expect_success(h2_lua_host_t *host, h2_lua_job_id_t job,
                           const char *message) {
  const h2_lua_job_status_t status = wait_job(host, job);
  if (status.state != H2_LUA_JOB_SUCCEEDED ||
      strcmp(status.message, message) != 0) {
    fprintf(stderr, "job %u: state=%d message=%s (expected %s)\n",
            (unsigned)job, (int)status.state, status.message, message);
    abort();
  }
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);
}

static void wait_until(int (*predicate)(void *), void *user) {
  for (int i = 0; i < 20000; ++i) {
    if (predicate(user)) {
      return;
    }
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  assert(!"condition not reached");
}

static int mark_reached(void *user) {
  return atomic_load((atomic_int *)user) > 0;
}

static int device_advertising(void *user) {
  const fake_snapshot_t value = fake_snapshot(user);
  return value.adv_running && value.registered;
}

static int device_released(void *user) {
  return fake_is_released(user);
}

static void wait_released(fake_device_t *device) {
  for (int i = 0; i < 20000 && !fake_is_released(device); ++i) {
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  if (!fake_is_released(device)) {
    const fake_snapshot_t v = fake_snapshot(device);
    fprintf(stderr,
            "device %d not released: sets=%d adv=%d scan=%d reg=%d conn=%d "
            "subs=%d/%d\n",
            device->index, v.adv_sets, v.adv_running, v.scanning, v.registered,
            v.connected, v.subscriptions, device->baseline_subscriptions);
    abort();
  }
}

/* ---- Lua scripts ---- */

#define LUA_PRELUDE                                                           \
  "local link=require('link');local rt=require('runtime');"                   \
  "local ev=rt.event;local s={msgs={}};"                                      \
  "s.dgrams={};"                                                               \
  "link.on(ev.LINK_CONNECTED,function(e) s.role=e.role;"                      \
  "s.max_datagram=e.max_datagram end);"                                        \
  "link.on(ev.LINK_MESSAGE,function(e) local q=e.reliable and s.msgs "        \
  "or s.dgrams;q[#q+1]=e.data end);"                                          \
  "link.on(ev.LINK_DISCONNECTED,function(e) s.disc=e.reason;"                 \
  "s.disc_result=e.result end);"                                               \
  "link.on(ev.LINK_ERROR,function(e) s.err=e.reason end);"                    \
  "local function wait(f) for _=1,4000 do if f() then return end "            \
  "rt.sleep(5) end error('wait timed out') end;"                              \
  "local function mark() require('capability').call('mark','{}') end;"        \
  "local function start(f,o) for _=1,600 do local ok,err=f(o);"               \
  "if ok then return end;assert(err=='link: busy',err);rt.sleep(5) end "      \
  "error('link stayed busy') end;"

static const char s_host_round_trip[] =
    LUA_PRELUDE
    "assert(link.available());"
    "local ok,err=link.send('early');assert(ok==nil and err=='link: not connected');"
    "assert(link.host({tag=args.tag}));"
    "assert(link.state()=='hosting');"
    "local busy,berr=link.join({tag=args.tag});assert(busy==nil and berr=='link: busy');"
    "wait(function() return s.role end);assert(s.role=='host');"
    "assert(link.state()=='connected');"
    "local big=string.rep('\\0\\1\\255',85)..'z';assert(#big==256);"
    "for i=1,3 do assert(link.send('ping-'..i)) end;assert(link.send(big));"
    "wait(function() return #s.msgs==4 end);"
    "for i=1,3 do assert(s.msgs[i]=='echo:ping-'..i,s.msgs[i]) end;"
    "assert(s.msgs[4]==big);"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "assert(link.state()=='idle');"
    "return 'host-ok'";

static const char s_join_round_trip[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));"
    "wait(function() return s.role end);assert(s.role=='join');"
    "wait(function() return #s.msgs==4 end);"
    "for i=1,3 do assert(link.send('echo:'..s.msgs[i])) end;"
    "assert(link.send(s.msgs[4]));"
    "assert(not pcall(link.send,string.rep('x',257)));"
    "assert(not pcall(link.send,''));"
    "rt.sleep(100);"
    "assert(link.close());assert(link.state()=='idle');"
    "return 'join-ok'";

static const char s_host_burst[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag}));"
    "wait(function() return s.role end);"
    "local busy=0;local pad=string.rep('p',190);"
    "for i=1,200 do while true do "
    "local ok,err=link.send(string.format('%04d',i)..pad);"
    "if ok then break end;assert(err=='link: busy',err);busy=busy+1;rt.sleep(2) "
    "end end;"
    "assert(busy>0,'sender never saw back-pressure');"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "return 'burst-ok'";

static const char s_join_burst[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));"
    "wait(function() return #s.msgs==200 end);"
    "for i=1,200 do assert(s.msgs[i]:sub(1,4)==string.format('%04d',i),"
    "'out of order at '..i) end;"
    "return 'burst-ok'";

static const char s_host_exit_on_first_message[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag}));"
    "wait(function() return #s.msgs>0 end);"
    "return 'exit-ok'";

static const char s_join_flood[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));"
    "wait(function() return s.role end);"
    "local i=0;"
    "while not s.disc do i=i+1;link.send('m'..i);rt.yield() end;"
    /* BYE is bounded best-effort: a saturated peer may see the disconnect
     * first. This case checks release and slot reuse, not the reason. */
    "assert(s.disc=='peer_closed' or s.disc=='lost',s.disc);"
    "return 'flood-ok'";

static const char s_reused_slot[] =
    LUA_PRELUDE
    "rt.sleep(30);"
    "assert(#s.msgs==0 and not s.role and not s.disc);"
    "assert(link.state()=='idle');"
    "return 'reuse-ok'";

/* Reliable messages, unreliable datagrams and the byte stream over one link. */
static const char s_host_transports[] =
    LUA_PRELUDE
    "local ok,err=link.send_unreliable('x');assert(ok==nil and err=='link: not connected');\n"
    "ok,err=link.write('x');assert(ok==nil and err=='link: not connected');\n"
    "assert(link.host({tag=args.tag}));\n"
    "wait(function() return s.role end);\n"
    "assert(s.max_datagram==244,s.max_datagram);\n"
    "assert(link.read(16,0)=='');\n"
    "assert(not pcall(link.send_unreliable,string.rep('d',245)));\n"
    "assert(not pcall(link.send_unreliable,''));\n"
    "assert(not pcall(link.read,0));\n"
    "for i=1,3 do assert(link.send_unreliable('u'..i)) end;\n"
    "local t={};for i=0,250 do t[#t+1]=string.char(i) end;\n"
    "local data=table.concat(t):rep(80):sub(1,20000);t=nil;\n"
    "local first=assert(link.write(data));local off=first+1;\n"
    "while off<=#data do local n=assert(link.write(data:sub(off,off+2047)));\n"
    "off=off+n;if n==0 then rt.sleep(1) end end;\n"
    "assert(first<#data,'stream never back-pressured');\n"
    "assert(link.send('stream-sent'));\n"
    "wait(function() return #s.msgs==1 and #s.dgrams==1 end);\n"
    "assert(s.msgs[1]=='got:20000',s.msgs[1]);assert(s.dgrams[1]=='j1');\n"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);\n"
    "assert(link.read(16,0)=='tail');\n"
    "local r,rerr=link.read(16,0);assert(r==nil and rerr=='link: closed',rerr);\n"
    "return 'transports-ok'";

static const char s_join_transports[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=5000}));\n"
    "wait(function() return s.role end);\n"
    "local total=0;\n"
    "while total<20000 do local chunk,rerr=link.read(4096,2000);\n"
    "assert(chunk,tostring(rerr)..' after '..total..' state '..link.state());\n"
    "for i=1,#chunk do assert(chunk:byte(i)==(total+i-1)%251,'stream byte '..total+i) end;\n"
    "total=total+#chunk end;\n"
    "wait(function() return #s.msgs==1 and #s.dgrams==3 end);\n"
    "assert(s.msgs[1]=='stream-sent');\n"
    "for i=1,3 do assert(s.dgrams[i]=='u'..i,s.dgrams[i]) end;\n"
    "local u,uerr=link.send_unreliable('j1');assert(u,'unreliable '..tostring(uerr));\n"
    "assert(link.write('tail')==4);\n"
    "assert(link.send('got:'..total));\n"
    "rt.sleep(100);\n"
    "return 'transports-ok'";

static const char s_wait_lost[] =
    LUA_PRELUDE
    "if args.tag=='lost-host' then assert(link.host({tag='lost'})) "
    "else assert(link.join({tag='lost',timeout_ms=5000})) end;"
    "wait(function() return s.role end);mark();"
    "wait(function() return s.disc end);assert(s.disc=='lost',s.disc);"
    "return 'lost-ok'";

static const char s_connect_then_idle[] =
    LUA_PRELUDE
    "if args.tag=='host' then start(link.host,{tag='idle'}) "
    "else start(link.join,{tag='idle',timeout_ms=5000}) end;"
    "wait(function() return s.role end);mark();"
    "while true do rt.sleep(10) end";

static const char s_wait_peer_closed[] =
    LUA_PRELUDE
    "start(link.host,{tag='idle'});"
    "wait(function() return s.role end);mark();"
    "wait(function() return s.disc end);assert(s.disc=='peer_closed',s.disc);"
    "return 'peer-closed-ok'";

/* host() straight from the LINK_DISCONNECTED callback must not see busy. */
static const char s_rehost_from_callback[] =
    LUA_PRELUDE
    "link.on(ev.LINK_DISCONNECTED,function(e) "
    "s.again={link.host({tag='again'})} end);"
    "start(link.host,{tag='idle'});"
    "wait(function() return s.role end);mark();"
    "wait(function() return s.again end);"
    "assert(s.again[1]==true,tostring(s.again[2]));"
    "assert(link.state()=='hosting');assert(link.close());"
    "return 'rehost-ok'";

static const char s_host_forever[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag}));mark();"
    "while true do rt.sleep(10) end";

static const char s_join_expect_not_found[] =
    LUA_PRELUDE
    "assert(link.join({tag=args.tag,timeout_ms=300}));"
    "wait(function() return s.err end);assert(s.err=='not_found',s.err);"
    "assert(link.state()=='idle');"
    "return 'not-found-ok'";

static const char s_host_timeout[] =
    LUA_PRELUDE
    "assert(link.host({tag=args.tag,timeout_ms=200}));"
    "wait(function() return s.err end);assert(s.err=='timeout',s.err);"
    "return 'host-timeout-ok'";

static const char s_unavailable[] =
    "local link=require('link');local rt=require('runtime');"
    "assert(link.available()==false);"
    "local ok,err=link.host({tag='x'});assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.join({tag='x'});assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.send('x');assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.send_unreliable('x');assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.write('x');assert(ok==nil and err=='link: unavailable');"
    "ok,err=link.read(16,0);assert(ok==nil and err=='link: unavailable');"
    "assert(link.close()==true);assert(link.state()=='unavailable');"
    "local h=link.on(rt.event.LINK_MESSAGE,function() end);"
    "assert(link.off(h)==true);"
    "assert(not pcall(link.on,12345,function() end));"
    "return 'unavailable-ok'";

static const char s_bad_options[] =
    "local link=require('link');"
    "assert(not pcall(link.host,{}));"
    "assert(not pcall(link.host,{tag=''}));"
    "assert(not pcall(link.host,{tag=string.rep('t',33)}));"
    "assert(not pcall(link.join,{tag='t',timeout_ms=0}));"
    "assert(not pcall(link.join,{tag='t',timeout_ms=60001}));"
    "assert(not pcall(link.host,'t'));"
    "return 'options-ok'";

/* ---- Tests ---- */

typedef struct pair {
  fake_air_t air;
  h2_runtime_t *runtime[2];
  h2_lua_host_t *host[2];
} pair_t;

static void pair_open(pair_t *pair) {
  fake_air_init(&pair->air);
  for (int i = 0; i < 2; ++i) {
    atomic_store(&s_marks[i], 0);
    pair->runtime[i] = fake_create_runtime(&pair->air.devices[i].ble,
                                      &pair->air.devices[i].events);
    fake_set_baseline(&pair->air.devices[i]);
    pair->host[i] = create_host(pair->runtime[i], 1, &s_marks[i]);
  }
}

static void pair_close(pair_t *pair) {
  for (int i = 0; i < 2; ++i) {
    if (pair->host[i] != NULL) {
      h2_lua_host_destroy(pair->host[i]);
    }
    assert(fake_is_released(&pair->air.devices[i]));
    h2_runtime_deinit(pair->runtime[i]);
  }
}

static void test_round_trip_and_peer_close(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host = submit(pair.host[0], "@host.lua", s_host_round_trip,
                                "tetris-duel");
  h2_lua_job_id_t join = submit(pair.host[1], "@join.lua", s_join_round_trip,
                                "tetris-duel");
  expect_success(pair.host[1], join, "join-ok");
  expect_success(pair.host[0], host, "host-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

static void test_three_transports(int hold_terminal) {
  pair_t pair;
  fake_terminal_gate_t gate = {
      .mutex = PTHREAD_MUTEX_INITIALIZER,
      .cond = PTHREAD_COND_INITIALIZER,
  };
  pair_open(&pair);
  pair.air.devices[0].terminal_gate = hold_terminal ? &gate : NULL;
  h2_lua_job_id_t host =
      submit(pair.host[0], "@transports.lua", s_host_transports, "duel");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@transports.lua", s_join_transports, "duel");
  if (hold_terminal) {
    wait_until(mark_reached, &gate.reached);
  }
  const h2_lua_job_status_t host_status = wait_job(pair.host[0], host);
  const h2_lua_job_status_t join_status = wait_job(pair.host[1], join);
  if (host_status.state != H2_LUA_JOB_SUCCEEDED ||
      join_status.state != H2_LUA_JOB_SUCCEEDED) {
    fprintf(stderr, "transports host=%d %s; join=%d %s\n",
            (int)host_status.state, host_status.message,
            (int)join_status.state, join_status.message);
  }
  /* Release even on job failure, before release/destroy can join the task. */
  pthread_mutex_lock(&gate.mutex);
  gate.released = 1;
  pthread_cond_broadcast(&gate.cond);
  pthread_mutex_unlock(&gate.mutex);
  expect_success(pair.host[0], host, "transports-ok");
  expect_success(pair.host[1], join, "transports-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
  pthread_cond_destroy(&gate.cond);
  pthread_mutex_destroy(&gate.mutex);
}

static void test_flow_control_keeps_order(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@burst.lua", s_host_burst, "burst");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@burst.lua", s_join_burst, "burst");
  expect_success(pair.host[1], join, "burst-ok");
  expect_success(pair.host[0], host, "burst-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

/* The host job ends and is released while the peer floods messages, and a
 * new job takes the same slot at once: no event or wake may reach it. */
static void test_release_during_traffic(void) {
  for (int round = 0; round < 10; ++round) {
    pair_t pair;
    pair_open(&pair);
    h2_lua_job_id_t host = submit(pair.host[0], "@exit.lua",
                                  s_host_exit_on_first_message, "flood");
    h2_lua_job_id_t join =
        submit(pair.host[1], "@flood.lua", s_join_flood, "flood");
    expect_success(pair.host[0], host, "exit-ok");
    h2_lua_job_id_t reused =
        submit(pair.host[0], "@reuse.lua", s_reused_slot, "flood");
    expect_success(pair.host[0], reused, "reuse-ok");
    expect_success(pair.host[1], join, "flood-ok");
    wait_until(device_released, &pair.air.devices[0]);
    wait_until(device_released, &pair.air.devices[1]);
    pair_close(&pair);
  }
}

/* Repeated sessions on the same devices, swapping roles, must reuse the one
 * retained link service instead of taking another GATT slot. */
static void test_sequential_sessions_reuse_service(void) {
  pair_t pair;
  pair_open(&pair);
  for (int round = 0; round < 4; ++round) {
    const int h = round % 2;
    atomic_store(&s_marks[0], 0);
    atomic_store(&s_marks[1], 0);
    h2_lua_job_id_t host =
        submit(pair.host[h], "@peer.lua", s_wait_peer_closed, "host");
    h2_lua_job_id_t join =
        submit(pair.host[1 - h], "@idle.lua", s_connect_then_idle, "join");
    for (int i = 0; i < 10000 && !(atomic_load(&s_marks[0]) &&
                                   atomic_load(&s_marks[1])); ++i) {
      (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
    }
    if (!(atomic_load(&s_marks[0]) && atomic_load(&s_marks[1]))) {
      h2_lua_job_status_t a;
      h2_lua_job_status_t b;
      assert(h2_lua_job_get_status(pair.host[h], host, &a) == H2_PAL_OK);
      assert(h2_lua_job_get_status(pair.host[1 - h], join, &b) == H2_PAL_OK);
      fprintf(stderr, "round %d: host state=%d msg=%s | join state=%d msg=%s\n",
              round, (int)a.state, a.message, (int)b.state, b.message);
      abort();
    }
    assert(h2_lua_job_cancel(pair.host[1 - h], join) == H2_PAL_OK);
    (void)wait_job(pair.host[1 - h], join);
    assert(h2_lua_job_release(pair.host[1 - h], join) == H2_PAL_OK);
    expect_success(pair.host[h], host, "peer-closed-ok");
    wait_released(&pair.air.devices[0]);
    wait_released(&pair.air.devices[1]);
  }
  for (int i = 0; i < 2; ++i) {
    pthread_mutex_lock(&pair.air.mutex);
    assert(pair.air.devices[i].registrations == 2);
    assert(pair.air.devices[i].retained_count == 2u);
    pthread_mutex_unlock(&pair.air.mutex);
  }
  pair_close(&pair);
}

static void test_rehost_from_disconnect_callback(void) {
  pair_t pair;
  pair_open(&pair);
  for (int round = 0; round < 5; ++round) {
    atomic_store(&s_marks[0], 0);
    atomic_store(&s_marks[1], 0);
    h2_lua_job_id_t host = submit(pair.host[0], "@rehost.lua",
                                  s_rehost_from_callback, "host");
    h2_lua_job_id_t join =
        submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
    wait_until(mark_reached, &s_marks[0]);
    wait_until(mark_reached, &s_marks[1]);
    assert(h2_lua_job_cancel(pair.host[1], join) == H2_PAL_OK);
    (void)wait_job(pair.host[1], join);
    assert(h2_lua_job_release(pair.host[1], join) == H2_PAL_OK);
    expect_success(pair.host[0], host, "rehost-ok");
    wait_released(&pair.air.devices[0]);
    wait_released(&pair.air.devices[1]);
  }
  pair_close(&pair);
}

static int device_scanning(void *user) {
  return fake_snapshot(user).scanning;
}

/* The joiner starts scanning first and sees the host's other advertisement,
 * which the duplicate filter then pins for that scan; restarting the scan
 * must still find the link once the host starts hosting. */
static void test_join_before_host_advertises(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t join =
      submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
  wait_until(device_scanning, &pair.air.devices[1]);
  (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 200u);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@peer.lua", s_wait_peer_closed, "host");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  pthread_mutex_lock(&pair.air.mutex);
  assert(pair.air.devices[1].scan_starts >= 2);
  pthread_mutex_unlock(&pair.air.mutex);
  assert(h2_lua_job_cancel(pair.host[1], join) == H2_PAL_OK);
  (void)wait_job(pair.host[1], join);
  assert(h2_lua_job_release(pair.host[1], join) == H2_PAL_OK);
  expect_success(pair.host[0], host, "peer-closed-ok");
  wait_released(&pair.air.devices[0]);
  wait_released(&pair.air.devices[1]);
  pair_close(&pair);
}

static void test_link_loss(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@lost.lua", s_wait_lost, "lost-host");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@lost.lua", s_wait_lost, "lost-join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  fake_drop_link(&pair.air);
  expect_success(pair.host[0], host, "lost-ok");
  expect_success(pair.host[1], join, "lost-ok");
  wait_until(device_released, &pair.air.devices[0]);
  wait_until(device_released, &pair.air.devices[1]);
  pair_close(&pair);
}

static void test_job_exit_releases_link(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@peer.lua", s_wait_peer_closed, "host");
  h2_lua_job_id_t join =
      submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  /* The joining app exits: its side releases BLE and the host sees BYE. */
  assert(h2_lua_job_cancel(pair.host[1], join) == H2_PAL_OK);
  assert(wait_job(pair.host[1], join).state == H2_LUA_JOB_CANCELLED);
  wait_until(device_released, &pair.air.devices[1]);
  expect_success(pair.host[0], host, "peer-closed-ok");
  wait_until(device_released, &pair.air.devices[0]);
  assert(h2_lua_job_release(pair.host[1], join) == H2_PAL_OK);
  pair_close(&pair);
}

static void test_host_destroy_releases_link(void) {
  pair_t pair;
  pair_open(&pair);
  (void)submit(pair.host[0], "@forever.lua", s_host_forever, "destroy");
  wait_until(mark_reached, &s_marks[0]);
  /* host() returns before the session task starts advertising. */
  wait_until(device_advertising, &pair.air.devices[0]);
  /* Destroy joins the session task: nothing is left once it returns. */
  h2_lua_host_destroy(pair.host[0]);
  pair.host[0] = NULL;
  assert(fake_is_released(&pair.air.devices[0]));

  /* A connected session is also torn down by destroy, and the peer sees
   * the BYE. */
  pair.host[0] = create_host(pair.runtime[0], 1, &s_marks[0]);
  atomic_store(&s_marks[0], 0);
  atomic_store(&s_marks[1], 0);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@peer.lua", s_wait_peer_closed, "host");
  (void)submit(pair.host[1], "@idle.lua", s_connect_then_idle, "join");
  wait_until(mark_reached, &s_marks[0]);
  wait_until(mark_reached, &s_marks[1]);
  h2_lua_host_destroy(pair.host[1]);
  pair.host[1] = NULL;
  assert(fake_is_released(&pair.air.devices[1]));
  expect_success(pair.host[0], host, "peer-closed-ok");
  pair_close(&pair);
}

static void test_tag_mismatch_and_timeouts(void) {
  pair_t pair;
  pair_open(&pair);
  h2_lua_job_id_t host =
      submit(pair.host[0], "@forever.lua", s_host_forever, "alpha");
  wait_until(mark_reached, &s_marks[0]);
  h2_lua_job_id_t join =
      submit(pair.host[1], "@join.lua", s_join_expect_not_found, "beta");
  expect_success(pair.host[1], join, "not-found-ok");
  /* The unrelated joiner never connected; the host keeps advertising. */
  assert(fake_snapshot(&pair.air.devices[0]).adv_running == 1);
  assert(fake_snapshot(&pair.air.devices[1]).connected == 0);
  assert(h2_lua_job_cancel(pair.host[0], host) == H2_PAL_OK);
  (void)wait_job(pair.host[0], host);
  assert(h2_lua_job_release(pair.host[0], host) == H2_PAL_OK);
  wait_until(device_released, &pair.air.devices[0]);

  host = submit(pair.host[0], "@timeout.lua", s_host_timeout, "gamma");
  expect_success(pair.host[0], host, "host-timeout-ok");
  wait_until(device_released, &pair.air.devices[0]);

  h2_lua_job_id_t options =
      submit(pair.host[0], "@options.lua", s_bad_options, "x");
  expect_success(pair.host[0], options, "options-ok");
  pair_close(&pair);
}

static void test_capability_off(void) {
  fake_air_t air;
  const h2_lua_link_config_t link_config = {
      .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
  };
  fake_air_init(&air);

  /* BLE present but the launcher did not enable the link. */
  h2_runtime_t *runtime = fake_create_runtime(&air.devices[0].ble,
                                         &air.devices[0].events);
  fake_set_baseline(&air.devices[0]);
  h2_lua_host_t *host = create_host(runtime, 0, NULL);
  expect_success(host, submit(host, "@off.lua", s_unavailable, "x"),
                 "unavailable-ok");
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_INVALID_STATE);
  h2_lua_host_destroy(host);
  assert(fake_is_released(&air.devices[0]));
  h2_runtime_deinit(runtime);

  /* A board without BLE: enable reports UNSUPPORTED and link stays off. */
  runtime = fake_create_runtime(h2_pal_unsupported_ble_host_api(),
                           h2_pal_unsupported_system_event_api());
  const h2_lua_host_config_t config = {.runtime = runtime};
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_link_enable(host, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_link_enable(host, &(h2_lua_link_config_t){
                                      .adv_type = (h2_pal_ble_adv_type_t)7,
                                  }) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  expect_success(host, submit(host, "@off.lua", s_unavailable, "x"),
                 "unavailable-ok");
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  /* BLE present but no system-event API. */
  runtime = fake_create_runtime(&air.devices[0].ble,
                           h2_pal_unsupported_system_event_api());
  const h2_lua_host_config_t no_events = {.runtime = runtime};
  assert(h2_lua_host_create(&no_events, &host) == H2_PAL_OK);
  assert(h2_lua_link_enable(host, &link_config) == H2_PAL_ERR_UNSUPPORTED);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

int main(void) {
  fprintf(stderr, "== test_capability_off\n");
  test_capability_off();
  fprintf(stderr, "== test_round_trip_and_peer_close\n");
  test_round_trip_and_peer_close();
  fprintf(stderr, "== test_three_transports\n");
  test_three_transports(0);
  fprintf(stderr, "== test_three_transports_before_task_exit\n");
  test_three_transports(1);
  fprintf(stderr, "== test_flow_control_keeps_order\n");
  test_flow_control_keeps_order();
  fprintf(stderr, "== test_release_during_traffic\n");
  test_release_during_traffic();
  fprintf(stderr, "== test_sequential_sessions_reuse_service\n");
  test_sequential_sessions_reuse_service();
  fprintf(stderr, "== test_rehost_from_disconnect_callback\n");
  test_rehost_from_disconnect_callback();
  fprintf(stderr, "== test_join_before_host_advertises\n");
  test_join_before_host_advertises();
  fprintf(stderr, "== test_link_loss\n");
  test_link_loss();
  fprintf(stderr, "== test_job_exit_releases_link\n");
  test_job_exit_releases_link();
  fprintf(stderr, "== test_host_destroy_releases_link\n");
  test_host_destroy_releases_link();
  fprintf(stderr, "== test_tag_mismatch_and_timeouts\n");
  test_tag_mismatch_and_timeouts();
  puts("lua link tests passed");
  return 0;
}
