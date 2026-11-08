#include "h2_device_service.h"
#include "h2loader_app_task_names.h"

#include <stdio.h>
#include <string.h>

#define H2_LOADER_COREDUMP_PARTITION_ID 3u
#define H2_LOADER_STAGE_PATH "/dl/update.tar.zlib"
#define H2_LOADER_STAGE_PREV_PATH "/dl/update.tar.zlib.prev"

static const char* event_name(h2_loader_startup_event_t event) {
  switch (event) {
    case H2_LOADER_STARTUP_EVENT_WRITE_PARTITION_2:
      return "write_partition_2";
    case H2_LOADER_STARTUP_EVENT_COPY_PARTITION_1:
      return "copy_partition_1";
    case H2_LOADER_STARTUP_EVENT_BOOT_PARTITION_2:
      return "boot_partition_2";
    case H2_LOADER_STARTUP_EVENT_STAGE_FINISHED:
      return "stage_finished";
    case H2_LOADER_STARTUP_EVENT_RECOVERY_FAILED:
      return "recovery_failed";
    default:
      return "unknown";
  }
}

static void on_event(void* user, h2_loader_startup_event_t event, int code) {
  h2_device_loader_t* context = (h2_device_loader_t*)user;
  if (context != NULL && context->runtime != NULL)
    (void)h2_pal_power_feed_watchdog(context->runtime->power);
  printf("H2_LOADER_STARTUP_EVENT event=%s code=%d\n", event_name(event), code);
  fflush(stdout);
  if (context == NULL || context->config == NULL) {
    return;
  }
  if ((event == H2_LOADER_STARTUP_EVENT_WRITE_PARTITION_2 ||
       event == H2_LOADER_STARTUP_EVENT_COPY_PARTITION_1) &&
      context->config->show_installing != NULL) {
    context->config->show_installing(context->config->user);
  } else if (event == H2_LOADER_STARTUP_EVENT_BOOT_PARTITION_2 &&
             context->config->show_launching != NULL) {
    context->config->show_launching(context->config->user);
  } else if (event == H2_LOADER_STARTUP_EVENT_RECOVERY_FAILED &&
             context->config->show_install_failed != NULL) {
    context->config->show_install_failed(context->config->user, code);
  }
}

static uint64_t now_ms(void* user) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_monotonic_ms(user, &value);
  return value;
}

static void sleep_ms(void* user, uint32_t delay_ms) {
  (void)h2_pal_time_sleep_ms(user, delay_ms);
}

static void loader_wifi_activity(void* user, bool active) {
  h2_device_loader_t* state = user;
  if (state->ble_service != NULL)
    (void)h2_loader_ble_service_request_advertising_paused(state->ble_service,
                                                           active);
}

typedef struct h2loader_ble_session {
  const h2_pal_task_api_t* task_api;
  h2_loader_command_config_t command_config;
  h2_loader_command_t command;
  h2_bleikcp_t* stream;
  int result;
} h2loader_ble_session_t;

static void command_task(void* ctx) {
  h2loader_ble_session_t* session = ctx;
  session->command_config.io = h2_loader_ble_command_io(session->stream);
  session->command_config.transport = H2_LOADER_COMMAND_TRANSPORT_BLE;
  session->result =
      h2_loader_command_init(&session->command, &session->command_config);
  while (session->result == H2_PAL_OK) {
    int rc = h2_loader_command_poll(&session->command, 50u);
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT ||
        rc == H2_PAL_ERR_WOULD_BLOCK) {
      continue;
    }
    if (rc == H2_PAL_ERR_CLOSED) {
      session->result = H2_PAL_OK;
      break;
    }
    session->result = rc;
  }
}

static int handle_session(void* user, h2_bleikcp_t* stream,
                          uint16_t conn_handle) {
  const h2_device_loader_t* base = user;
  h2loader_ble_session_t session;
  (void)conn_handle;
  if (base == NULL || stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  session = (h2loader_ble_session_t){
      .task_api = base->runtime->task,
      .command_config = base->ble_command,
      .stream = stream,
      .result = H2_PAL_OK,
  };
  command_task(&session);
  return session.result;
}

static int open_command_service(
    void* user, h2_runtime_t* runtime,
    const h2_loader_command_config_t* command_config, const char* board,
    uint32_t capabilities, void** out_service) {
  h2_device_loader_t* owner = user;
  if (owner == NULL || runtime == NULL || command_config == NULL ||
      board == NULL || out_service == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_service = NULL;
  int init_rc = h2_bleikcp_global_init();
  if (init_rc != H2_PAL_OK) return init_rc;
  owner->ble_command = *command_config;
  const h2_loader_ble_service_config_t service_config = {
      .log = runtime->log,
      .api =
          {
              .ble = runtime->ble_host,
              .task = runtime->task,
              .time = runtime->time,
              .sync = runtime->sync,
              .system_event = runtime->system_event,
              .allocator = runtime->mem,
          },
      .board = board,
      .local_name = runtime->ble_local_name,
      .capabilities = capabilities,
      .advertising_mode = owner->config->ble_advertising_mode,
      .handler = handle_session,
      .handler_user = owner,
      .command_stack_size = 49152u,
  };
  h2_loader_ble_service_t* service = NULL;
  int rc = h2_loader_ble_service_open(&service_config, &service);
  if (rc == H2_PAL_OK) {
    owner->ble_service = service;
    (void)h2_device_activity_bind(&owner->wifi_activity, runtime->wifi_sta,
                                  runtime->time, loader_wifi_activity, owner);
    *out_service = service;
  } else {
    (void)h2_bleikcp_global_shutdown();
  }
  return rc;
}

static int close_command_service(void* user, void* service) {
  h2_device_loader_t* owner = user;
  if (service == NULL) {
    return H2_PAL_OK;
  }
  h2_device_activity_unbind(&owner->wifi_activity);
  int rc = h2_loader_ble_service_close(service);
  if (rc == H2_PAL_OK) owner->ble_service = NULL;
  if (rc == H2_PAL_OK) (void)h2_bleikcp_global_shutdown();
  return rc;
}

static int runtime_fs_clear_adapter(void* user, const char* path) {
  return h2_pal_fs_clear((const h2_pal_fs_api_t*)user, path);
}

static int prepare_loader(void* user, h2_loader_t* loader) {
  h2_device_loader_t* context = (h2_device_loader_t*)user;
  h2_runtime_t* runtime = context->runtime;
  (void)loader;
  int rc = h2_pal_fs_prepare(runtime->fs, "/dl");
  if (rc == H2_PAL_OK) rc = h2_pal_fs_prepare(runtime->fs, "/data");
  if (rc == H2_PAL_OK)
    rc = h2_loader_package_recover_publish(runtime->fs, runtime->pref,
                                           runtime->mem, H2_LOADER_STAGE_PATH,
                                           H2_LOADER_STAGE_PREV_PATH);
  return rc == H2_PAL_OK && context->config->force_command_mode
             ? H2_PAL_ERR_INVALID_STATE
             : rc;
}

static int serve_loader(void* user, h2_loader_t* loader,
                        h2_loader_command_t* command,
                        h2_loader_startup_action_t action) {
  h2_loader_command_config_t command_config = command->config;
  int rc;

  h2_device_loader_t* context = user;
  if (context->config->command_ready != NULL)
    context->config->command_ready(context->config->user, loader);
  printf("H2_LOADER_READY status=ready action=%d\n", (int)action);
  fflush(stdout);
  while (!h2_atomic_bool_load(&context->stop_requested, H2_ATOMIC_ACQUIRE)) {
    if (context->serial.pending_conv != 0u) {
      rc = h2_loader_command_init(command, &command_config);
      if (rc == H2_PAL_OK) {
        rc = h2_device_serial_activate_pending(&context->serial);
      }
      if (rc != H2_PAL_OK) {
        printf("H2_LOADER_ERROR reason=session_activate_failed code=%d\n", rc);
        fflush(stdout);
      }
      continue;
    }
    if (!h2_device_serial_has_session(&context->serial)) {
      rc = h2_device_serial_poll_session(&context->serial, 50u);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_WOULD_BLOCK) {
        printf("H2_LOADER_ERROR reason=session_poll_failed code=%d\n", rc);
        fflush(stdout);
      }
      continue;
    }
    rc = h2_loader_command_poll(command, 50u);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
      continue;
    }
    if (rc == H2_PAL_ERR_CLOSED &&
        h2_device_serial_replacement_pending(&context->serial)) {
      if (h2_device_serial_close_pending(&context->serial) &&
          context->serial.pending_conv == 0u) {
        h2_device_serial_deactivate(&context->serial);
      }
      continue;
    }
    if (rc != H2_PAL_OK) {
      printf("H2_LOADER_ERROR reason=command_failed code=%d\n", rc);
    }
    fflush(stdout);
  }
  return H2_PAL_OK;
}

static int stop_serve_loader(void* user) {
  h2_device_loader_t* context = user;
  h2_atomic_bool_store(&context->stop_requested, true, H2_ATOMIC_RELEASE);
  return H2_PAL_OK;
}

static int loader_run(h2_device_loader_t* context, h2_runtime_t* runtime,
                      const h2_device_loader_config_t* runtime_loader_config,
                      const h2_iostreamikcp_io_t* physical_io, int with_ble) {
  h2loader_app_config_t config;
  int rc;
  const char* board;
  char device_uid[H2_LOADER_DEVICE_UID_MAX];
  h2_pal_firmware_info_t firmware_info;

  if (context == NULL || runtime == NULL || runtime_loader_config == NULL ||
      runtime->fs == NULL || runtime->pref == NULL || runtime->power == NULL ||
      runtime->mem == NULL || runtime->image == NULL || runtime->time == NULL ||
      runtime->task == NULL || runtime->sync == NULL ||
      runtime->firmware_info == NULL || physical_io == NULL) {
    printf("H2_LOADER_READY status=runtime_config_fail\n");
    return H2_PAL_ERR_INVALID_ARG;
  }
  board = runtime_loader_config->board != NULL ? runtime_loader_config->board
                                               : runtime->board;
  rc = h2_loader_pal_device_uid(runtime->device_identity, device_uid);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_boot_policy_config_t boot_config = {
      .app_entry_path = runtime_loader_config->app_entry_path,
      .fs = runtime->fs,
      .pref = runtime->pref,
      .image = runtime->image,
      .power = runtime->power,
      .time = runtime->time,
      .allocator = runtime_loader_config->command_allocator != NULL
                       ? runtime_loader_config->command_allocator
                       : (runtime->mem_external != NULL ? runtime->mem_external
                                                        : runtime->mem),
      .loader_partition_id = runtime_loader_config->loader_partition_id != 0u
                                 ? runtime_loader_config->loader_partition_id
                                 : 1u,
      .app_partition_id = runtime_loader_config->app_partition_id != 0u
                              ? runtime_loader_config->app_partition_id
                              : 2u,
      .digest_yield_ms = runtime_loader_config->digest_yield_ms,
      .digest_feed_watchdog = 1,
      .flags = runtime_loader_config->boot_policy_flags,
      .loader_identity = runtime_loader_config->identity_policy,
  };
  rc = h2_device_boot_policy_init(&context->boot, &boot_config);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &firmware_info);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY status=firmware_info_fail code=%d\n", rc);
    return rc;
  }
  context->runtime = runtime;
  context->config = runtime_loader_config;
  if (h2_atomic_bool_init(&context->stop_requested, false) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  const h2_device_serial_config_t serial = {
      .physical_io = *physical_io,
      .allocator = boot_config.allocator,
      .time = runtime->time,
      .stop_requested = &context->stop_requested,
      .idle_timeout_ms = runtime_loader_config->serial_idle_timeout_ms,
      .post_write_poll_ms = runtime_loader_config->serial_post_write_poll_ms,
  };
  rc = h2_device_serial_init_with_config(&context->serial, &serial);
  if (rc != H2_PAL_OK) {
    h2_atomic_bool_destroy(&context->stop_requested);
    return rc;
  }
  memset(&config, 0, sizeof(config));
  config.loader.package.fs = runtime->fs;
  config.loader.package.disk = runtime->disk;
  config.loader.package.allocator = boot_config.allocator;
  config.loader.package.app_entry_path = runtime_loader_config->app_entry_path;
  config.loader.package.digest = context->boot.pal.digest;
  config.loader.package.clear_data = runtime_fs_clear_adapter;
  config.loader.package.clear_data_user = (void*)runtime->fs;
  config.loader.package.image_reader = &context->boot.pal.image_reader;
  config.loader.package.image_writer = &context->boot.pal.image_writer;
  config.loader.package.progress_user = runtime_loader_config->user;
  config.loader.package.progress = runtime_loader_config->install_progress;
  config.loader.board = board;
  config.loader.target = runtime->target;
  config.loader.device_uid = device_uid;
  config.loader.power = &context->boot.power;
  config.loader.h2loader_partition_id = boot_config.loader_partition_id;
  config.loader.app_partition_id = boot_config.app_partition_id;
  config.loader.mfg_required_total = runtime_loader_config->mfg_required_total;
  config.loader.hardware_capabilities =
      runtime_loader_config->hardware_capabilities != 0u
          ? runtime_loader_config->hardware_capabilities
          : H2_LOADER_CAPABILITIES_ALL;
  rc = h2_device_boot_policy_current_identity(
      &context->boot, H2_LOADER_IMAGE_ROLE_H2LOADER, board, runtime->target,
      firmware_info.version, &config.loader.active_identity);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY status=identity_fail code=%d\n", rc);
    h2_device_serial_deinit(&context->serial);
    h2_atomic_bool_destroy(&context->stop_requested);
    return rc;
  }
  config.loader.confirm_user = &context->boot;
  config.loader.confirm_active_image = h2_device_boot_policy_confirm;
  config.loader.mount_user = (void*)runtime->fs;
  config.loader.mount_file_point = h2_loader_pal_prepare_file_point;
  config.loader.event_user = context;
  config.loader.on_event = on_event;
  config.loader.disruptive_user = runtime_loader_config->user;
  config.loader.before_disruptive = runtime_loader_config->before_disruptive;
  config.command.digest = config.loader.package.digest;
  config.command.memory_stats.user = (void*)runtime->mem;
  config.command.memory_stats.read = h2_loader_pal_memory_stats_read;
  config.command.clock_user = (void*)runtime->time;
  config.command.now_ms = now_ms;
  config.command.sleep_ms = sleep_ms;
  config.command.io = h2_device_serial_io(&context->serial);
  config.command.coredump_partition_id =
      runtime_loader_config->coredump_partition_id != 0u
          ? runtime_loader_config->coredump_partition_id
          : H2_LOADER_COREDUMP_PARTITION_ID;
  config.command.mfg_user = runtime_loader_config->user;
  config.command.mfg_command = runtime_loader_config->mfg_command;
  config.user = context;
  config.prepare = prepare_loader;
  config.before_startup_user = runtime_loader_config->user;
  config.before_startup = runtime_loader_config->run_mfg;
  config.rearm_before_startup = runtime_loader_config->rearm_mfg;
  config.stop_serve = stop_serve_loader;
  config.serve = serve_loader;
  if (runtime_loader_config->extend_app != NULL) {
    rc = runtime_loader_config->extend_app(runtime, &config);
    if (rc != H2_PAL_OK) {
      h2_device_serial_deinit(&context->serial);
      h2_atomic_bool_destroy(&context->stop_requested);
      return rc;
    }
  }
  const h2loader_app_command_service_api_t command_service = {
      .user = context,
      .open = open_command_service,
      .close = close_command_service,
  };
  rc = h2loader_app_run_with_command_service(
      runtime, &config,
      runtime_loader_config->command_service != NULL
          ? runtime_loader_config->command_service
          : (with_ble ? &command_service : NULL));
  h2_atomic_bool_destroy(&context->stop_requested);
  h2_device_serial_deinit(&context->serial);
  printf("H2_LOADER_READY status=app_exit code=%d\n", rc);
  return rc;
}

int h2_device_loader_run(h2_device_loader_t* context, h2_runtime_t* runtime,
                         const h2_device_loader_config_t* config,
                         const h2_iostreamikcp_io_t* physical_io,
                         int with_ble) {
  if (context == NULL || runtime == NULL || config == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (physical_io != NULL)
    return loader_run(context, runtime, config, physical_io, with_ble);
  int rc = h2_pal_io_stream_start(runtime->io_stream);
  if (rc != H2_PAL_OK) return rc;
  h2_iostreamikcp_io_t io = h2_iostreamikcp_io_from_stream(runtime->io_stream);
  rc = loader_run(context, runtime, config, &io, with_ble);
  /* The blocking App has joined every borrower before returning. */
  int released = h2_pal_io_stream_release(runtime->io_stream);
  if (rc == H2_PAL_OK) rc = released;
  return rc;
}
