#include "h2_pal_e2e_application_host.h"
#if defined(H2_PAL_E2E_HOST_TEST)
#include "h2_pal_e2e_host_test_sdk.h"
#elif defined(H2_PAL_E2E_ESP)
#include "h2_esp_board.h"
#include "h2_esp_board_application_host.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#else
#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_board_application_host.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#endif
#include <stdio.h>
#include <string.h>

#if defined(H2_PAL_E2E_ESP)
#define BOARD_PROVIDER h2_esp_application_host_t
#define BOARD_INIT h2_esp_board_application_host_init
#define DEFAULT_ALLOCATOR h2_esp_platform_default_allocator
#define PLATFORM_TIME h2_esp_platform_time_api
#define STOP_SERVICES h2_esp_h2loader_app_commands_stop_serial
#define SERIAL_STARTED h2_esp_h2loader_app_serial_is_started
#define START_ENTRY h2_esp_board_start_entry_task
#else
#define BOARD_PROVIDER h2_bk_application_host_t
#define BOARD_INIT h2_bk_board_application_host_init
#define DEFAULT_ALLOCATOR h2_bk_platform_default_allocator
#define PLATFORM_TIME h2_bk_platform_time_api
#define STOP_SERVICES h2_bk_h2loader_stop_app_commands
#define SERIAL_STARTED h2_bk_h2loader_app_serial_is_started
#define START_ENTRY h2_bk7258_board_start_entry_task
#endif

void h2_pal_e2e_application_host_park(void) {
  for (;;)
    (void)h2_pal_time_sleep_ms(PLATFORM_TIME(), 1000u);
}

void h2_pal_e2e_application_host_fail(const char *stage, h2_pal_result_t result) {
  printf("H2_PAL_E2E_HOST stage=%s application=%d\n", stage, result);
  fflush(stdout);
  h2_pal_e2e_application_host_park();
}

typedef struct launcher {
  BOARD_PROVIDER board;
  const char *name;
  uint32_t capabilities;
  h2_application_host_config_t config;
  h2_application_host_t *host;
} launcher_t;

static h2_pal_result_t before_prepare(void *user) {
  launcher_t *l = user;
  return l->config.before_prepare(l->config.user);
}

static h2_pal_result_t prepare(void *user, struct h2_runtime_config *out) {
  launcher_t *l = user;
  return l->board.api.vtable->prepare(l->board.api.user, out);
}
static h2_pal_result_t activate(void *user, struct h2_runtime *runtime) {
  launcher_t *l = user;
  return l->board.api.vtable->activate(l->board.api.user, runtime);
}
static h2_pal_result_t poll(void *user, int *out_stop) {
  launcher_t *l = user;
  return l->board.api.vtable->poll(l->board.api.user, out_stop);
}
static h2_pal_result_t deactivate(void *user) {
  launcher_t *l = user;
  h2_pal_result_t rc = STOP_SERVICES();
  return rc == H2_PAL_OK ? l->board.api.vtable->deactivate(l->board.api.user)
                         : rc;
}
static h2_pal_result_t release(void *user) {
  launcher_t *l = user;
  h2_pal_result_t rc = STOP_SERVICES();
  return rc == H2_PAL_OK ? l->board.api.vtable->release(l->board.api.user) : rc;
}
static void notify(void *user, h2_pal_application_host_event_t event) {
  launcher_t *l = user;
  l->board.api.vtable->notify(l->board.api.user, event);
}
static const h2_pal_application_host_vtable_t vtable = {.prepare = prepare,
                                                        .activate = activate,
                                                        .poll = poll,
                                                        .deactivate =
                                                            deactivate,
                                                        .release = release,
                                                        .notify = notify};

static h2_pal_result_t startup_error(launcher_t *l, h2_pal_result_t rc) {
  if (rc != H2_PAL_OK) {
    h2_pal_result_t cleanup = STOP_SERVICES();
    if (cleanup != H2_PAL_OK)
      (void)h2_application_host_retain(l->host, l, cleanup);
  }
  return rc;
}
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  launcher_t *l = user;
  h2_pal_result_t rc = l->config.configure
                           ? l->config.configure(l->config.user, config)
                           : H2_PAL_OK;
#if defined(H2_PAL_E2E_ESP)
  if (rc == H2_PAL_OK && l->name && !SERIAL_STARTED()) {
    const h2_esp_h2loader_app_commands_config_t commands = {
        .active_name = l->name,
        .hardware_capabilities = l->capabilities,
        .h2loader_partition_id = 1u,
        .coredump_partition_id = 3u};
    rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(config,
                                                                 &commands);
  }
#endif
  return startup_error(l, rc);
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  launcher_t *l = user;
  h2_pal_result_t rc = l->config.validate
                           ? l->config.validate(l->config.user, runtime)
                           : H2_PAL_OK;
#if !defined(H2_PAL_E2E_ESP)
  if (rc == H2_PAL_OK && l->name && !SERIAL_STARTED())
    rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
        runtime, l->name, l->capabilities);
#endif
  return startup_error(l, rc);
}
static h2_pal_result_t release_runtime(void *user) {
  launcher_t *l = user;
  h2_pal_result_t rc = STOP_SERVICES();
  if (rc != H2_PAL_OK)
    return rc;
  return l->config.release_runtime ? l->config.release_runtime(l->config.user)
                                   : H2_PAL_OK;
}
static h2_pal_result_t entry(h2_application_host_t *host, h2_runtime_t *runtime,
                             void *user) {
  launcher_t *l = user;
  h2_pal_result_t rc = h2_application_host_ready(host);
  if (rc != H2_PAL_OK)
    return rc;
  rc = l->config.entry(host, runtime, l->config.user);
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_E2E_HOST name=%s application=%d waiting_for_stop=1\n",
           l->name ? l->name : "-", rc);
    fflush(stdout);
    /* Failed fixtures may still borrow Runtime. Keep serial recovery alive
     * until Stop, then retain the graph before the App task returns. */
    (void)h2_pal_e2e_application_host_wait(host, runtime);
  }
  return h2_pal_e2e_application_host_finish(host, runtime, rc);
}
h2_pal_result_t h2_pal_e2e_application_host_wait(h2_application_host_t *host,
                                                 h2_runtime_t *runtime) {
  while (!h2_application_host_should_stop(host)) {
    h2_pal_result_t rc = h2_pal_time_sleep_ms(runtime->time, 10u);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_pal_e2e_application_host_finish(h2_application_host_t *host,
                                                   h2_runtime_t *runtime,
                                                   h2_pal_result_t result) {
  if (result != H2_PAL_OK)
    (void)h2_application_host_retain(host, runtime, result);
  return result;
}
h2_pal_result_t
h2_pal_e2e_application_host_run(const char *name, uint32_t capabilities,
                                const h2_application_host_config_t *config,
                                h2_application_host_result_t *result) {
  h2_application_host_result_t local_result = {0};
  if (!result)
    result = &local_result;
  *result = local_result;
  if (!config || !config->entry || config->provider ||
      (name && (!name[0] || !(capabilities & H2_LOADER_CAPABILITY_UART) ||
                (capabilities & ~H2_LOADER_CAPABILITIES_ALL))))
    return H2_PAL_ERR_INVALID_ARG;
  if (SERIAL_STARTED())
    return H2_PAL_ERR_INVALID_STATE;
  launcher_t l = {
      .config = *config, .name = name, .capabilities = capabilities};
  h2_pal_result_t rc = BOARD_INIT(&l.board);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_application_host_api_t provider = {.user = &l,
                                                  .vtable = &vtable};
  h2_application_host_config_t actual = *config;
  actual.allocator =
      config->allocator ? config->allocator : DEFAULT_ALLOCATOR();
  actual.provider = &provider;
  actual.user = &l;
  actual.before_prepare = config->before_prepare ? before_prepare : NULL;
  actual.configure = configure;
  actual.validate = validate;
  actual.release_runtime = release_runtime;
  actual.entry = entry;
  rc = h2_application_host_create(&actual, &l.host);
  if (rc == H2_PAL_OK)
    rc = h2_application_host_run(l.host, result);
  if (result->retained) {
    printf("H2_PAL_E2E_HOST application=%d cleanup=%d retained=1\n",
           result->application, result->cleanup);
    fflush(stdout);
    /* BSP entry wrappers may release providers when their owner returns. */
    h2_pal_e2e_application_host_park();
  }
  h2_pal_result_t destroyed = h2_application_host_destroy(&l.host);
  return rc == H2_PAL_OK ? destroyed : rc;
}

typedef struct native_start {
  h2_pal_mem_api_t allocator;
  h2_application_host_config_t config;
  uint32_t capabilities;
  char name[];
} native_start_t;

static void native_entry(void *user) {
  native_start_t *start = user;
  h2_pal_mem_api_t allocator = start->allocator;
  h2_pal_result_t rc = h2_pal_e2e_application_host_run(
      start->name, start->capabilities, &start->config, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_E2E_HOST name=%s startup=%d\n", start->name, rc);
    fflush(stdout);
  }
  h2_pal_mem_free(&allocator, start);
  if (rc != H2_PAL_OK)
    h2_pal_e2e_application_host_park();
}

h2_pal_result_t h2_pal_e2e_application_host_start(
    const char *name, uint32_t capabilities,
    const h2_application_host_config_t *config) {
  if (!name || !name[0] || !config || !config->entry || config->provider ||
      !(capabilities & H2_LOADER_CAPABILITY_UART) ||
      (capabilities & ~H2_LOADER_CAPABILITIES_ALL))
    return H2_PAL_ERR_INVALID_ARG;
#if defined(H2_PAL_E2E_ESP)
  h2_pal_result_t rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    h2_pal_e2e_application_host_fail(name, rc);
#else
  h2_pal_result_t rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK)
    return rc;
  bk_init();
#endif
  const h2_pal_mem_api_t *allocator = DEFAULT_ALLOCATOR();
  size_t name_size = strlen(name);
  if (name_size >= SIZE_MAX - sizeof(native_start_t))
    return H2_PAL_ERR_INVALID_ARG;
  ++name_size;
  native_start_t *start =
      h2_pal_mem_alloc(allocator, sizeof(*start) + name_size);
  if (!start) {
    printf("H2_PAL_E2E_HOST name=%s startup=%d\n", name, H2_PAL_ERR_NO_MEMORY);
    fflush(stdout);
    return H2_PAL_ERR_NO_MEMORY;
  }
  start->allocator = *allocator;
  start->config = *config;
  start->capabilities = capabilities;
  memcpy(start->name, name, name_size);
  rc = START_ENTRY(start->name, native_entry, start);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, start);
    printf("H2_PAL_E2E_HOST name=%s startup=%d\n", name, rc);
    fflush(stdout);
  }
  return rc;
}
