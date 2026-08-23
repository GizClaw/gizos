#include "h2_lua_runtime_e2e.h"

#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_lua_vm.h"
#include "h2_runtime_test.h"

#include <string.h>

static const char *const s_case_ids[H2_LUA_RUNTIME_E2E_CASE_COUNT] = {
    "vm-source-load",      "coroutine-api",        "coroutine-concurrency",
    "timer-wakeup",        "component-lookup",     "component-event",
    "cancel-timeout-race", "multi-vm-concurrency", "shutdown-with-waiters",
};

static h2_pal_result_t
list_components(void *user, h2_runtime_component_t filter,
                h2_runtime_component_mapping_cb_t callback,
                void *callback_user) {
  static const h2_runtime_component_mapping_entry_t entries[] = {
      {H2_LUA_RUNTIME_E2E_COMPONENT_ID, H2_LUA_RUNTIME_E2E_COMPONENT_ID},
      {H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u,
       H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u},
  };
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_BUZZER)
    return H2_PAL_OK;
  for (size_t i = 0u; i < sizeof(entries) / sizeof(entries[0]); ++i) {
    h2_pal_result_t result = callback(callback_user, &entries[i]);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t get_periph_id(void *user,
                                     h2_runtime_component_id_t component_id,
                                     h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id != H2_LUA_RUNTIME_E2E_COMPONENT_ID &&
      component_id != H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u)
    return H2_PAL_ERR_NOT_FOUND;
  *out_periph_id = component_id;
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = list_components,
    .get_periph_id = get_periph_id,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};

static h2_pal_result_t list_peripherals(void *user, h2_pal_periph_type_t filter,
                                        h2_pal_periph_cb_t callback,
                                        void *callback_user) {
  static const h2_pal_periph_info_t infos[] = {
      {.id = H2_LUA_RUNTIME_E2E_COMPONENT_ID,
       .type = H2_PAL_PERIPH_TYPE_BUZZER,
       .name = "lua-e2e-event"},
      {.id = H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u,
       .type = H2_PAL_PERIPH_TYPE_BUZZER,
       .name = "lua-e2e-unmatched-event"},
  };
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY && filter != H2_PAL_PERIPH_TYPE_BUZZER)
    return H2_PAL_OK;
  for (size_t i = 0u; i < sizeof(infos) / sizeof(infos[0]); ++i) {
    h2_pal_result_t result = callback(callback_user, &infos[i]);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t get_peripheral(void *user, h2_pal_periph_id_t id,
                                      h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id != H2_LUA_RUNTIME_E2E_COMPONENT_ID &&
      id != H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = (h2_pal_periph_info_t){
      .id = id,
      .type = H2_PAL_PERIPH_TYPE_BUZZER,
  };
  strcpy(out_info->name, id == H2_LUA_RUNTIME_E2E_COMPONENT_ID
                             ? "lua-e2e-event"
                             : "lua-e2e-unmatched-event");
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = list_peripherals,
    .get = get_peripheral,
};

static const h2_pal_periph_api_t s_periph_api = {
    .vtable = &s_periph_vtable,
};

const h2_runtime_component_mapper_t *h2_lua_runtime_e2e_component_mapper(void) {
  return &s_mapper;
}

const h2_pal_periph_api_t *h2_lua_runtime_e2e_periph_api(void) {
  return &s_periph_api;
}

static h2_pal_result_t create_host(h2_runtime_t *runtime, size_t worker_count,
                                   size_t max_jobs, size_t event_capacity,
                                   uint32_t timeout_ms,
                                   h2_lua_host_t **out_host) {
  h2_pal_result_t result = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = worker_count < max_jobs ? worker_count : max_jobs,
          .max_jobs = max_jobs,
          .event_delivery_capacity = event_capacity,
          .callback_capacity_per_job = 8u,
          .max_coroutines_per_vm = 40u,
          .vm_memory_limit_bytes = 256u * 1024u,
          .source_limit_bytes = 8192u,
          .output_limit_bytes = 256u,
          .instruction_quantum = 1000u,
          .execution_timeout_ms = timeout_ms,
      },
      out_host);
  return result == H2_PAL_OK ? h2_lua_host_start(*out_host) : result;
}

static int terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static h2_pal_result_t wait_for_state(h2_runtime_t *runtime,
                                      h2_lua_host_t *host, h2_lua_job_id_t id,
                                      h2_lua_job_state_t wanted,
                                      int terminal_is_enough,
                                      h2_lua_job_status_t *out_status) {
  for (size_t attempt = 0u; attempt < 2000u; ++attempt) {
    h2_pal_result_t result = h2_lua_job_get_status(host, id, out_status);
    if (result != H2_PAL_OK)
      return result;
    if (out_status->state == wanted ||
        (terminal_is_enough && terminal(out_status->state))) {
      return H2_PAL_OK;
    }
    (void)h2_lua_host_step(host);
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t wait_for_progress_while_waiting(
    h2_runtime_t *runtime, h2_lua_host_t *host, h2_lua_job_id_t id,
    uint64_t resume_count, uint32_t initial_delay_ms,
    h2_lua_job_status_t *out_status) {
  h2_pal_result_t result = H2_PAL_OK;
  if (initial_delay_ms > 0u)
    result = h2_pal_time_sleep_ms(runtime->time, initial_delay_ms);
  for (size_t attempt = 0u; result == H2_PAL_OK && attempt < 2000u;
       ++attempt) {
    result = h2_lua_job_get_status(host, id, out_status);
    if (result != H2_PAL_OK)
      return result;
    if (terminal(out_status->state))
      return H2_PAL_ERR_INVALID_STATE;
    if (out_status->resume_count > resume_count) {
      return out_status->state == H2_LUA_JOB_WAITING
                 ? H2_PAL_OK
                 : H2_PAL_ERR_INVALID_STATE;
    }
    (void)h2_lua_host_step(host);
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  return result == H2_PAL_OK ? H2_PAL_ERR_TIMEOUT : result;
}

static h2_pal_result_t run_script(h2_runtime_t *runtime, const char *script,
                                  const char *expected, uint64_t *out_resumes) {
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t id = 0u;
  h2_lua_job_status_t status = {0};
  h2_pal_result_t result = create_host(runtime, 1u, 1u, 2u, 1000u, &host);
  if (result == H2_PAL_OK) {
    result = h2_lua_job_submit_text(host, "@e2e.lua", (const uint8_t *)script,
                                    strlen(script), NULL, 0u, &id);
  }
  if (result == H2_PAL_OK) {
    result =
        wait_for_state(runtime, host, id, H2_LUA_JOB_SUCCEEDED, 1, &status);
  }
  if (result == H2_PAL_OK) {
    result = status.state == H2_LUA_JOB_SUCCEEDED &&
                     strcmp(status.message, expected) == 0
                 ? H2_PAL_OK
                 : H2_PAL_ERR_INVALID_STATE;
    *out_resumes = status.resume_count;
  }
  h2_lua_host_destroy(host);
  return result;
}

static h2_pal_result_t case_vm_source(h2_runtime_t *runtime,
                                      uint64_t *evidence) {
  static const uint8_t resource_source[] = "return 42";
  static const h2_lua_resource_t resources[] = {
      {"@fixture.lua", resource_source, sizeof(resource_source) - 1u},
  };
  h2_lua_vm_t *vm = NULL;
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t id = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status = {0};
  h2_lua_vm_execution_t execution;
  char output[16];
  char error[64];
  static const uint8_t source[] = "return 42";
  static const uint8_t malformed[] = "return )";
  static const uint8_t oversized[129] = {0};
  static const uint8_t bytecode[] = {0x1bu, 'L', 'u', 'a'};
  h2_lua_vm_config_t config = {
      .memory_limit_bytes = 128u * 1024u,
      .source_limit_bytes = 128u,
      .output_limit_bytes = 15u,
  };
  execution = (h2_lua_vm_execution_t){output, sizeof(output), 0u,
                                      error,  sizeof(error),  0u};
  if (h2_lua_vm_create(&config, &vm) != H2_LUA_VM_OK ||
      h2_lua_vm_execute_text(vm, "@buffer.lua", source, sizeof(source) - 1u,
                             &execution) != H2_LUA_VM_OK ||
      strcmp(output, "42") != 0 ||
      h2_lua_vm_execute_text(vm, "@bytecode.lua", bytecode, sizeof(bytecode),
                             &execution) != H2_LUA_VM_BYTECODE_REJECTED ||
      h2_lua_vm_execute_text(vm, "@malformed.lua", malformed,
                             sizeof(malformed) - 1u,
                             &execution) != H2_LUA_VM_SYNTAX_ERROR ||
      h2_lua_vm_execute_text(vm, "@oversized.lua", oversized, sizeof(oversized),
                             &execution) != H2_LUA_VM_SOURCE_TOO_LARGE) {
    h2_lua_vm_close(vm);
    return H2_PAL_ERR_INVALID_STATE;
  }
  *evidence = h2_lua_vm_memory_used(vm);
  h2_lua_vm_close(vm);
  h2_pal_result_t result = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = 1u,
          .max_jobs = 1u,
          .event_delivery_capacity = 1u,
          .max_coroutines_per_vm = 2u,
          .vm_memory_limit_bytes = 128u * 1024u,
          .source_limit_bytes = 128u,
          .output_limit_bytes = 15u,
          .instruction_quantum = 1000u,
          .execution_timeout_ms = 1000u,
          .resources = resources,
          .resource_count = sizeof(resources) / sizeof(resources[0]),
      },
      &host);
  if (result == H2_PAL_OK)
    result = h2_lua_host_start(host);
  if (result == H2_PAL_OK)
    result = h2_lua_job_submit_resource(host, "@fixture.lua", NULL, 0u, &id);
  if (result == H2_PAL_OK)
    result =
        wait_for_state(runtime, host, id, H2_LUA_JOB_SUCCEEDED, 1, &status);
  if (result == H2_PAL_OK && (status.state != H2_LUA_JOB_SUCCEEDED ||
                              strcmp(status.message, "42") != 0)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  h2_lua_host_destroy(host);
  return result;
}

static h2_pal_result_t case_script(h2_runtime_t *runtime, size_t index,
                                   uint64_t *evidence) {
  static const char *const scripts[] = {
      NULL,
      "local c=coroutine.create(function()"
      "local iy=coroutine.isyieldable();coroutine.yield(7,iy);return 9 end);"
      "local s0=coroutine.status(c)=='suspended';"
      "local a,x,iy=coroutine.resume(c);"
      "local s1=coroutine.status(c)=='suspended';"
      "local b,y=coroutine.resume(c);local s2=coroutine.status(c)=='dead';"
      "local d=coroutine.create(function()coroutine.yield()end);"
      "coroutine.resume(d);local closed=coroutine.close(d);"
      "local wrapped=coroutine.wrap(function()return 5 end)();"
      "return s0 and a and x==7 and iy and s1 and b and y==9 and s2 and "
      "closed and wrapped==5 and 'coroutine-ok' or 'bad'",
      "local a=require('runtime');local t={};"
      "local x=a.spawn(function()t[#t+1]='a';a.yield();t[#t+1]='A'end);"
      "local y=a.spawn(function()t[#t+1]='b';a.yield();t[#t+1]='B'end);"
      "a.join(x);a.join(y);return table.concat(t)",
      "local a=require('runtime');local n=0;"
      "local long=a.spawn(function()a.sleep(1000)end);"
      "local tick=a.spawn(function()for i=1,10 do n=n+1;a.sleep(10)end end);"
      "a.join(tick);a.cancel(long);return n==10 and 'ticker-ok' or 'bad'",
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
  };
  static const char *const expected[] = {
      NULL, "coroutine-ok", "abAB", "ticker-ok", NULL, NULL, NULL, NULL, NULL,
  };
  return run_script(runtime, scripts[index], expected[index], evidence);
}

typedef union runtime_event_payload {
  h2_pal_result_t error_alignment;
  uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} runtime_event_payload_t;

static h2_pal_result_t app_consume_and_dispatch_event(h2_runtime_t *runtime,
                                                      h2_lua_host_t *host,
                                                      h2_lua_job_id_t job_id) {
  runtime_event_payload_t payload;
  h2_runtime_event_t event = {
      .payload = payload.bytes,
      .payload_capacity = sizeof(payload.bytes),
  };
  h2_pal_result_t result = h2_runtime_poll_event(runtime, &event);
  if (result != H2_PAL_OK)
    return result;
  result = h2_lua_dispatch_runtime_event(host, job_id, &event);

  event = (h2_runtime_event_t){
      .payload = payload.bytes,
      .payload_capacity = sizeof(payload.bytes),
  };
  h2_pal_result_t empty_result = h2_runtime_poll_event(runtime, &event);
  if (empty_result != H2_PAL_ERR_WOULD_BLOCK &&
      empty_result != H2_PAL_ERR_TIMEOUT) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return result;
}

static h2_pal_result_t app_inject_consume_and_dispatch_event(
    h2_runtime_test_control_t *control, h2_runtime_t *runtime,
    h2_lua_host_t *host, h2_lua_job_id_t job_id,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms, h2_pal_result_t error) {
  h2_pal_result_t result = h2_runtime_test_emit_event(
      control, H2_RUNTIME_COMPONENT_EVENT_ERROR, H2_RUNTIME_COMPONENT_BUZZER,
      component_id, timestamp_ms, &error, sizeof(error));
  return result == H2_PAL_OK
             ? app_consume_and_dispatch_event(runtime, host, job_id)
             : result;
}

static h2_pal_result_t case_component_lookup(h2_runtime_t *runtime,
                                             uint64_t *evidence) {
  static const char script[] =
      "local r=require('runtime');"
      "local unavailable,e1=r.components.get(23);"
      "local missing,e2=r.components.get(999);"
      "return unavailable==nil and type(e1)=='string' and missing==nil and "
      "type(e2)=='string' and 'lookup-ok' or 'bad'";
  h2_runtime_component_info_t info = {0};
  h2_pal_result_t result =
      h2_runtime_component_get(runtime, H2_LUA_RUNTIME_E2E_COMPONENT_ID, &info);
  if (result != H2_PAL_OK || info.kind != H2_RUNTIME_COMPONENT_BUZZER ||
      info.component_id != H2_LUA_RUNTIME_E2E_COMPONENT_ID) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  result = run_script(runtime, script, "lookup-ok", evidence);
  if (result == H2_PAL_OK)
    *evidence = (uint64_t)info.kind;
  return result;
}

static h2_pal_result_t case_event(h2_runtime_t *runtime, uint64_t *evidence) {
  static const char script[] =
      "local r=require('runtime');local n=0;local gate;"
      "r.components.on(23,r.event.ERROR,function(e)"
      "if e.result==-1 then n=n+1;r.cancel(gate) end end);"
      "gate=r.spawn(function()while true do r.sleep(1) end end);"
      "r.join(gate);return tostring(n)";
  h2_lua_host_t *host = NULL;
  h2_runtime_test_control_t *control = NULL;
  h2_lua_job_id_t id = 0u;
  h2_lua_job_status_t status = {0};
  h2_pal_result_t error = H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t result = create_host(runtime, 1u, 1u, 2u, 1000u, &host);
  if (result == H2_PAL_OK) {
    result = h2_lua_job_submit_text(host, "@event.lua", (const uint8_t *)script,
                                    strlen(script), NULL, 0u, &id);
  }
  if (result == H2_PAL_OK) {
    result = wait_for_state(runtime, host, id, H2_LUA_JOB_WAITING, 0, &status);
  }
  if (result == H2_PAL_OK) {
    result = wait_for_progress_while_waiting(runtime, host, id,
                                             status.resume_count, 10u, &status);
  }
  if (result == H2_PAL_OK)
    result = h2_runtime_test_control_open(runtime, &control);
  if (result == H2_PAL_OK) {
    result = app_inject_consume_and_dispatch_event(
        control, runtime, host, id, H2_LUA_RUNTIME_E2E_COMPONENT_ID + 1u, 1u,
        error);
    if (result == H2_PAL_OK) {
      result = h2_lua_job_get_status(host, id, &status);
    }
    if (result == H2_PAL_OK) {
      result = wait_for_progress_while_waiting(
          runtime, host, id, status.resume_count, 0u, &status);
    }
  }
  if (result == H2_PAL_OK) {
    result = app_inject_consume_and_dispatch_event(
        control, runtime, host, id, H2_LUA_RUNTIME_E2E_COMPONENT_ID, 2u, error);
  }
  if (result == H2_PAL_OK) {
    result =
        wait_for_state(runtime, host, id, H2_LUA_JOB_SUCCEEDED, 1, &status);
    if (result == H2_PAL_OK && (status.state != H2_LUA_JOB_SUCCEEDED ||
                                strcmp(status.message, "1") != 0)) {
      result = H2_PAL_ERR_INVALID_STATE;
    }
    *evidence = status.resume_count;
  }
  h2_runtime_test_control_close(control);
  h2_lua_host_destroy(host);
  return result;
}

static h2_pal_result_t case_cancel_timeout(h2_runtime_t *runtime,
                                           uint64_t *evidence) {
  static const char script[] = "while true do end";
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t id = 0u;
  h2_lua_job_status_t status = {0};
  h2_pal_result_t result = create_host(runtime, 1u, 1u, 1u, 2u, &host);
  if (result == H2_PAL_OK) {
    result =
        h2_lua_job_submit_text(host, "@timeout.lua", (const uint8_t *)script,
                               strlen(script), NULL, 0u, &id);
  }
  if (result == H2_PAL_OK) {
    (void)h2_pal_time_sleep_ms(runtime->time, 1u);
    (void)h2_lua_job_cancel(host, id);
    result =
        wait_for_state(runtime, host, id, H2_LUA_JOB_TIMED_OUT, 1, &status);
  }
  if (result == H2_PAL_OK && status.state != H2_LUA_JOB_TIMED_OUT &&
      status.state != H2_LUA_JOB_CANCELLED) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  *evidence = status.resume_count;
  h2_lua_host_destroy(host);
  return result;
}

static h2_pal_result_t case_multi_vm(h2_runtime_t *runtime, size_t worker_count,
                                     uint64_t *evidence) {
  static const char script[] =
      "local d=require('delay');d.delay_ms(1);return args.id";
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t ids[4] = {0};
  char values[4][2] = {{'1', 0}, {'2', 0}, {'3', 0}, {'4', 0}};
  size_t i;
  size_t complete = 0u;
  h2_pal_result_t result =
      create_host(runtime, worker_count, 4u, 1u, 1000u, &host);
  for (i = 0u; result == H2_PAL_OK && i < 4u; ++i) {
    h2_lua_arg_t arg = {"id", values[i]};
    result = h2_lua_job_submit_text(host, "@multi.lua", (const uint8_t *)script,
                                    strlen(script), &arg, 1u, &ids[i]);
  }
  if (result == H2_PAL_OK)
    result = h2_lua_host_step(host);
  for (i = 0u; result == H2_PAL_OK && i < 2000u; ++i) {
    size_t job_index;
    size_t terminal_count = 0u;
    result = h2_lua_host_step(host);
    for (job_index = 0u; result == H2_PAL_OK && job_index < 4u; ++job_index) {
      h2_lua_job_status_t status;
      result = h2_lua_job_get_status(host, ids[job_index], &status);
      if (result == H2_PAL_OK && terminal(status.state))
        terminal_count++;
    }
    if (terminal_count == 4u)
      break;
    (void)h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  for (i = 0u; result == H2_PAL_OK && i < 4u; ++i) {
    h2_lua_job_status_t status;
    result = h2_lua_job_get_status(host, ids[i], &status);
    if (result == H2_PAL_OK && status.state == H2_LUA_JOB_SUCCEEDED &&
        strcmp(status.message, values[i]) == 0) {
      complete++;
    }
  }
  *evidence = complete;
  h2_lua_host_destroy(host);
  return result == H2_PAL_OK && complete == 4u ? H2_PAL_OK
                                               : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t case_shutdown(h2_runtime_t *runtime,
                                     uint64_t *evidence) {
  static const char script[] = "require('delay').delay_ms(1000);return 'late'";
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t id = 0u;
  h2_lua_job_status_t status = {0};
  h2_pal_result_t result = create_host(runtime, 1u, 1u, 1u, 5000u, &host);
  if (result == H2_PAL_OK) {
    result = h2_lua_job_submit_text(host, "@wait.lua", (const uint8_t *)script,
                                    strlen(script), NULL, 0u, &id);
  }
  if (result == H2_PAL_OK)
    result = h2_lua_host_step(host);
  if (result == H2_PAL_OK)
    result = h2_lua_host_stop(host);
  if (result == H2_PAL_OK)
    result = h2_lua_host_join(host);
  if (result == H2_PAL_OK)
    result = h2_lua_job_get_status(host, id, &status);
  *evidence = status.resume_count;
  h2_lua_host_destroy(host);
  return result == H2_PAL_OK && status.state == H2_LUA_JOB_STOPPED
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_lua_runtime_e2e_run(h2_runtime_t *runtime,
                       const h2_lua_runtime_e2e_config_t *config,
                       h2_lua_runtime_e2e_report_t *out_report) {
  size_t i;
  if (runtime == NULL || config == NULL || config->scheduler == NULL ||
      config->worker_count == 0u || out_report == NULL ||
      runtime->mem == NULL || runtime->time == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_report, 0, sizeof(*out_report));
  out_report->scheduler = config->scheduler;
  out_report->case_count = H2_LUA_RUNTIME_E2E_CASE_COUNT;
  for (i = 0u; i < H2_LUA_RUNTIME_E2E_CASE_COUNT; ++i) {
    h2_lua_runtime_e2e_case_result_t *case_result = &out_report->cases[i];
    case_result->id = s_case_ids[i];
    if (i == 0u) {
      case_result->result = case_vm_source(runtime, &case_result->evidence);
    } else if (i == 4u) {
      case_result->result =
          case_component_lookup(runtime, &case_result->evidence);
    } else if (i == 5u) {
      case_result->result = case_event(runtime, &case_result->evidence);
    } else if (i == 6u) {
      case_result->result =
          case_cancel_timeout(runtime, &case_result->evidence);
    } else if (i == 7u) {
      case_result->result =
          case_multi_vm(runtime, config->worker_count, &case_result->evidence);
    } else if (i == 8u) {
      case_result->result = case_shutdown(runtime, &case_result->evidence);
    } else {
      case_result->result = case_script(runtime, i, &case_result->evidence);
    }
    if (case_result->result == H2_PAL_OK)
      out_report->passed++;
    if (config->report_case != NULL)
      config->report_case(config->report_case_user, case_result);
  }
  return out_report->passed == out_report->case_count
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}
