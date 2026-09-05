#include "h2_gizclaw_e2e.h"

#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_e2e_internal.h"
#include "h2_gizclaw_e2e_report.h"
#include "h2_gizclaw_e2e_task_names.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct progress_state {
  h2_runtime_t *runtime;
  h2_gizclaw_e2e_progress_fn on_progress;
  void *progress_user;
  h2_pal_mutex_t *mutex;
  const char *active_case;
  uint64_t started_ms;
  uint32_t interval_ms;
  size_t selected;
  size_t terminal;
  size_t passed;
  size_t failed;
  size_t errors;
  size_t blocked;
  size_t cancelled;
} progress_state_t;

typedef struct run_control {
  h2_runtime_t *runtime;
  const h2_gizclaw_e2e_config_t *config;
  progress_state_t progress;
  h2_gizclaw_e2e_report_t report;
  h2_pal_task_t *task;
  int aggregate_cleanup_rc;
  size_t retained_resources;
  char runtime_profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  h2_gizclaw_e2e_fixture_t *retained_fixture;
  atomic_bool exited;
} run_control_t;

static atomic_flag s_run_active = ATOMIC_FLAG_INIT;

static uint32_t value_or_default(uint32_t value, uint32_t fallback) {
  return value == 0u ? fallback : value;
}

static uint32_t progress_interval(uint32_t configured_ms) {
  if (configured_ms == 0u ||
      configured_ms > H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS) {
    return H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS;
  }
  return configured_ms;
}

static bool config_valid(h2_runtime_t *runtime,
                         const h2_gizclaw_e2e_config_t *config,
                         h2_gizclaw_e2e_result_t *out_result) {
  uint32_t supported_suites = 0u;
  for (size_t i = 0u; i < h2_gizclaw_e2e_case_count; ++i)
    supported_suites |= h2_gizclaw_e2e_cases[i].suite;
  if (runtime == NULL || config == NULL || out_result == NULL ||
      runtime->mem == NULL || runtime->crypto == NULL ||
      runtime->http == NULL || runtime->log == NULL || runtime->time == NULL ||
      runtime->task == NULL || runtime->sync == NULL ||
      runtime->queue == NULL || runtime->webrtc == NULL ||
      config->server_endpoint.data == NULL ||
      config->server_endpoint.len == 0u ||
      config->server_endpoint.len >= H2_GIZCLAW_E2E_ENDPOINT_CAPACITY ||
      config->registration_token.data == NULL ||
      config->registration_token.len == 0u ||
      config->registration_token.len > H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX ||
      config->suites == 0u ||
      (config->suites & ~H2_GIZCLAW_E2E_SUITE_ALL) != 0u ||
      (config->suites & ~supported_suites) != 0u ||
      memchr(config->server_endpoint.data, '\0', config->server_endpoint.len) !=
          NULL ||
      memchr(config->registration_token.data, '\0',
             config->registration_token.len) != NULL) {
    return false;
  }
  const bool needs_voice =
      (config->suites &
       (H2_GIZCLAW_E2E_SUITE_RPC | H2_GIZCLAW_E2E_SUITE_VOICE)) != 0u;
  if (needs_voice &&
      (config->voice_pcm_s16le_16khz_mono == NULL ||
       config->voice_pcm_len == 0u || config->voice_pcm_len > 1024u * 1024u ||
       (config->voice_pcm_len % 2u) != 0u)) {
    return false;
  }
  if (needs_voice) {
    bool non_silent = false;
    for (size_t index = 0u; index < config->voice_pcm_len; ++index) {
      non_silent =
          non_silent || config->voice_pcm_s16le_16khz_mono[index] != 0u;
    }
    if (!non_silent) {
      return false;
    }
  }
  return true;
}

static void progress_emit(progress_state_t *state,
                          h2_gizclaw_e2e_progress_kind_t kind,
                          const char *case_id, const char *status,
                          const char *blocked_by, int rc, bool complete,
                          bool notify_observer) {
  h2_gizclaw_e2e_progress_t progress = {
      .kind = kind,
      .case_id = case_id,
      .status = status,
      .blocked_by = blocked_by,
      .rc = rc,
      .complete = complete,
  };
  if (state->mutex != NULL) {
    (void)h2_pal_mutex_lock(state->runtime->sync, state->mutex);
  }
  progress.selected = state->selected;
  progress.terminal = state->terminal;
  progress.passed = state->passed;
  progress.failed = state->failed;
  progress.errors = state->errors;
  progress.blocked = state->blocked;
  progress.cancelled = state->cancelled;
  if (state->mutex != NULL) {
    (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  }
  uint64_t now_ms = state->started_ms;
  if (h2_pal_time_get_monotonic_ms(state->runtime->time, &now_ms) ==
      H2_PAL_OK) {
    progress.elapsed_ms = h2_pal_time_elapsed_ms(state->started_ms, now_ms);
  }
  if (notify_observer && state->on_progress != NULL) {
    state->on_progress(state->progress_user, &progress);
  }
  char message[H2_PAL_LOG_MESSAGE_MAX];
  (void)snprintf(message, sizeof(message),
                 "kind=%u case=%s status=%s rc=%d terminal=%zu/%zu "
                 "complete=%s",
                 (unsigned)kind, case_id == NULL ? "-" : case_id,
                 status == NULL ? "-" : status, rc, progress.terminal,
                 progress.selected, complete ? "true" : "false");
  (void)h2_pal_log_write(state->runtime->log,
                         rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN,
                         "gizclaw-e2e", message);
}

static int progress_init(progress_state_t *state, h2_runtime_t *runtime,
                         const h2_gizclaw_e2e_config_t *config,
                         size_t selected) {
  memset(state, 0, sizeof(*state));
  state->runtime = runtime;
  state->on_progress = config->on_progress;
  state->progress_user = config->progress_user;
  state->selected = selected;
  state->interval_ms = progress_interval(config->progress_interval_ms);
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &state->started_ms);
  const h2_pal_mutex_config_t mutex_config = {
      .name = "gizclaw-e2e-progress",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  if (rc == H2_PAL_OK) {
    rc = h2_pal_mutex_create(runtime->sync, &mutex_config, &state->mutex);
  }
  if (rc != H2_PAL_OK && state->mutex != NULL) {
    (void)h2_pal_mutex_destroy(runtime->sync, state->mutex);
    state->mutex = NULL;
  }
  return rc;
}

static void copy_summary(const h2_gizclaw_e2e_summary_t *summary,
                         const char *runtime_profile_name,
                         h2_gizclaw_e2e_result_t *out_result) {
  out_result->selected = summary->selected;
  out_result->terminal = summary->terminal;
  out_result->passed = summary->pass;
  out_result->failed = summary->fail;
  out_result->errors = summary->error;
  out_result->blocked = summary->blocked;
  out_result->cancelled = summary->cancelled;
  out_result->first_failure_rc = summary->first_failure_rc;
  out_result->cleanup_rc = summary->cleanup_rc;
  out_result->complete = summary->complete;
  if (summary->first_failure_case != NULL) {
    (void)snprintf(out_result->first_failure_case,
                   sizeof(out_result->first_failure_case), "%s",
                   summary->first_failure_case);
  }
  if (runtime_profile_name != NULL) {
    (void)snprintf(out_result->runtime_profile_name,
                   sizeof(out_result->runtime_profile_name), "%s",
                   runtime_profile_name);
  }
}

static h2_gizclaw_e2e_exit_t report_start_failure(
    h2_runtime_t *runtime, const h2_gizclaw_e2e_config_t *config,
    h2_gizclaw_e2e_result_t *out_result, int rc, size_t retained_resources) {
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  for (size_t index = 0u; index < h2_gizclaw_e2e_case_count; ++index) {
    if ((config->suites & h2_gizclaw_e2e_cases[index].suite) != 0u) {
      (void)h2_gizclaw_e2e_report_select(&report,
                                         h2_gizclaw_e2e_cases[index].id);
      (void)h2_gizclaw_e2e_report_terminal(&report,
                                           h2_gizclaw_e2e_cases[index].id,
                                           H2_GIZCLAW_E2E_CASE_ERROR, rc, NULL);
    }
  }

  progress_state_t progress = {
      .runtime = runtime,
      .on_progress = config->on_progress,
      .progress_user = config->progress_user,
      .selected = report.selected,
      .interval_ms = progress_interval(config->progress_interval_ms),
  };
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &progress.started_ms);
  for (size_t index = 0u; index < report.selected; ++index) {
    progress.terminal++;
    progress.errors++;
    progress_emit(&progress, H2_GIZCLAW_E2E_PROGRESS_CASE,
                  report.cases[index].id, "ERROR", NULL, rc, false, true);
  }
  progress_emit(&progress, H2_GIZCLAW_E2E_PROGRESS_CLEANUP, NULL, "ERROR", NULL,
                rc, false, true);
  h2_gizclaw_e2e_report_cleanup(&report, rc);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&report);
  copy_summary(&summary, NULL, out_result);
  out_result->retained_resources = retained_resources;
  progress_emit(&progress, H2_GIZCLAW_E2E_PROGRESS_SUMMARY, NULL, "SUMMARY",
                NULL, summary.first_failure_rc, summary.complete, true);
  return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
}

static void run_cases_task(void *user) {
  run_control_t *control = user;
  h2_runtime_t *runtime = control->runtime;
  const h2_gizclaw_e2e_config_t *config = control->config;
  progress_state_t *progress = &control->progress;
  h2_gizclaw_e2e_report_t *report = &control->report;
  int aggregate_cleanup_rc = H2_PAL_OK;
  size_t retained_resources = 0u;

  for (size_t index = 0u; index < h2_gizclaw_e2e_case_count; ++index) {
    const e2e_case_t *test_case = &h2_gizclaw_e2e_cases[index];
    if ((config->suites & test_case->suite) == 0u) {
      continue;
    }
    const bool stopped = config->should_stop != NULL &&
                         config->should_stop(config->should_stop_user);
    h2_gizclaw_e2e_case_status_t status =
        stopped ? H2_GIZCLAW_E2E_CASE_CANCELLED : H2_GIZCLAW_E2E_CASE_PASS;
    int case_rc = stopped ? H2_PAL_ERR_CLOSED : H2_PAL_OK;

    (void)h2_pal_mutex_lock(runtime->sync, progress->mutex);
    progress->active_case = test_case->id;
    (void)h2_pal_mutex_unlock(runtime->sync, progress->mutex);
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=%s\n", test_case->id);

    h2_gizclaw_e2e_fixture_t *fixture = NULL;
    bool fixture_initialized = false;
    if (!stopped) {
      if (control->retained_fixture != NULL) {
        case_rc = H2_PAL_ERR_INVALID_STATE;
      } else {
        fixture = h2_pal_mem_alloc(runtime->mem, sizeof(*fixture));
        if (fixture != NULL)
          memset(fixture, 0, sizeof(*fixture));
        else
          case_rc = H2_PAL_ERR_NO_MEMORY;
      }
      if (case_rc == H2_PAL_OK)
        case_rc = h2_gizclaw_e2e_fixture_init(
            fixture, runtime, config,
            value_or_default(config->case_timeout_ms,
                             H2_GIZCLAW_E2E_DEFAULT_CASE_TIMEOUT_MS));
      fixture_initialized = case_rc == H2_PAL_OK;
      if (case_rc == H2_PAL_OK && test_case->needs_voice &&
          (fixture->pcm == NULL || fixture->pcm_len == 0u)) {
        case_rc = H2_PAL_ERR_INVALID_ARG;
      }
      if (case_rc == H2_PAL_OK) {
        case_rc = h2_gizclaw_e2e_fixture_connect_actors(fixture,
                                                        test_case->actor_count);
      }
      if (case_rc == H2_PAL_OK) {
        case_rc = test_case->run(fixture);
      }
      status = case_rc == H2_PAL_OK ? H2_GIZCLAW_E2E_CASE_PASS
                                    : H2_GIZCLAW_E2E_CASE_FAIL;
      if (fixture != NULL && fixture->runtime_profile_name[0] != '\0') {
        if (control->runtime_profile_name[0] == '\0') {
          (void)snprintf(control->runtime_profile_name,
                         sizeof(control->runtime_profile_name), "%s",
                         fixture->runtime_profile_name);
        } else if (strcmp(control->runtime_profile_name,
                          fixture->runtime_profile_name) != 0) {
          case_rc = H2_PAL_ERR_INVALID_STATE;
          status = H2_GIZCLAW_E2E_CASE_ERROR;
        }
      }
    }

    int cleanup_rc = H2_PAL_OK;
    if (fixture_initialized) {
      cleanup_rc = h2_gizclaw_e2e_fixture_set_deadline(
          fixture, value_or_default(config->cleanup_timeout_ms,
                                    H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS));
      if (cleanup_rc == H2_PAL_OK) {
        cleanup_rc = h2_gizclaw_e2e_fixture_cleanup(fixture);
      }
      const size_t retained =
          h2_gizclaw_e2e_fixture_emit_recovery_ledger(fixture);
      retained_resources += retained;
      if (cleanup_rc == H2_PAL_OK && retained != 0u)
        cleanup_rc = H2_PAL_ERR_INVALID_STATE;
      const int deinit_rc = h2_gizclaw_e2e_fixture_deinit(fixture);
      if (deinit_rc != H2_PAL_OK) {
        if (cleanup_rc == H2_PAL_OK)
          cleanup_rc = deinit_rc;
        /* A PAL join failure must never turn borrowed state into stack garbage.
         * Fail closed: retain the owner and do not start another case. */
        control->retained_fixture = fixture;
        ++retained_resources;
        fixture = NULL;
      }
    }
    h2_pal_mem_free(runtime->mem, fixture);
    if (aggregate_cleanup_rc == H2_PAL_OK && cleanup_rc != H2_PAL_OK) {
      aggregate_cleanup_rc = cleanup_rc;
    }
    (void)h2_gizclaw_e2e_report_terminal(report, test_case->id, status, case_rc,
                                         NULL);
    printf("H2_GIZCLAW_E2E stage=coverage-end case=%s status=%s rc=%d "
           "cleanup_rc=%d\n",
           test_case->id, h2_gizclaw_e2e_case_status_name(status), case_rc,
           cleanup_rc);

    (void)h2_pal_mutex_lock(runtime->sync, progress->mutex);
    progress->active_case = NULL;
    progress->terminal++;
    progress->passed += status == H2_GIZCLAW_E2E_CASE_PASS;
    progress->failed += status == H2_GIZCLAW_E2E_CASE_FAIL;
    progress->errors += status == H2_GIZCLAW_E2E_CASE_ERROR;
    progress->blocked += status == H2_GIZCLAW_E2E_CASE_BLOCKED;
    progress->cancelled += status == H2_GIZCLAW_E2E_CASE_CANCELLED;
    (void)h2_pal_mutex_unlock(runtime->sync, progress->mutex);
  }

  control->aggregate_cleanup_rc = aggregate_cleanup_rc;
  control->retained_resources = retained_resources;
  atomic_store_explicit(&control->exited, true, memory_order_release);
}

static int join_runner(run_control_t *control, uint32_t cleanup_timeout_ms,
                       size_t *out_retained_resources) {
  uint64_t attempts = ((uint64_t)cleanup_timeout_ms + 9u) / 10u;
  while (control->task != NULL && attempts > 0u) {
    const int rc = h2_pal_task_join(control->runtime->task, control->task);
    attempts--;
    if (rc == H2_PAL_OK) {
      control->task = NULL;
      break;
    }
    if (attempts > 0u) {
      (void)h2_pal_time_sleep_ms(control->runtime->time, 10u);
    }
  }
  int rc = control->task == NULL ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
  if (control->task == NULL) {
    const int destroy_rc =
        h2_pal_mutex_destroy(control->runtime->sync, control->progress.mutex);
    if (destroy_rc == H2_PAL_OK) {
      control->progress.mutex = NULL;
    }
    if (rc == H2_PAL_OK) {
      rc = destroy_rc;
    }
  }
  *out_retained_resources = (control->task == NULL ? 0u : 1u) +
                            (control->progress.mutex == NULL ? 0u : 1u);
  if (*out_retained_resources != 0u) {
    (*out_retained_resources)++;
  }
  return rc;
}

h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_run(h2_runtime_t *runtime,
                                         const h2_gizclaw_e2e_config_t *config,
                                         h2_gizclaw_e2e_result_t *out_result) {
  if (out_result != NULL) {
    memset(out_result, 0, sizeof(*out_result));
  }
  if (!config_valid(runtime, config, out_result) ||
      atomic_flag_test_and_set_explicit(&s_run_active, memory_order_acquire)) {
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  run_control_t *control = h2_pal_mem_alloc(runtime->mem, sizeof(*control));
  if (control == NULL) {
    const h2_gizclaw_e2e_exit_t exit = report_start_failure(
        runtime, config, out_result, H2_PAL_ERR_NO_MEMORY, 0u);
    atomic_flag_clear_explicit(&s_run_active, memory_order_release);
    return exit;
  }
  memset(control, 0, sizeof(*control));
  control->runtime = runtime;
  control->config = config;
  atomic_init(&control->exited, false);
  h2_gizclaw_e2e_report_init(&control->report);
  for (size_t index = 0u; index < h2_gizclaw_e2e_case_count; ++index) {
    if ((config->suites & h2_gizclaw_e2e_cases[index].suite) != 0u) {
      (void)h2_gizclaw_e2e_report_select(&control->report,
                                         h2_gizclaw_e2e_cases[index].id);
    }
  }

  int rc = progress_init(&control->progress, runtime, config,
                         control->report.selected);
  const h2_pal_task_options_t task_options = {
      .name = h2_gizclaw_e2e_runner_task_name,
      .min_stack_size = 32768u,
  };
  if (rc == H2_PAL_OK) {
    rc = h2_pal_task_start(runtime->task, &task_options, run_cases_task,
                           control, &control->task);
  }
  if (rc != H2_PAL_OK) {
    size_t retained_resources = 0u;
    if (control->progress.mutex != NULL) {
      const int destroy_rc =
          h2_pal_mutex_destroy(runtime->sync, control->progress.mutex);
      if (destroy_rc != H2_PAL_OK) {
        retained_resources = 2u;
        rc = destroy_rc;
      } else {
        control->progress.mutex = NULL;
      }
    }
    const h2_gizclaw_e2e_exit_t exit = report_start_failure(
        runtime, config, out_result, rc, retained_resources);
    if (retained_resources == 0u) {
      h2_pal_mem_free(runtime->mem, control);
      atomic_flag_clear_explicit(&s_run_active, memory_order_release);
    } else {
      control->runtime = NULL;
      control->config = NULL;
      control->progress.runtime = NULL;
      control->progress.on_progress = NULL;
      control->progress.progress_user = NULL;
    }
    return exit;
  }

  uint64_t last_emit_ms = control->progress.started_ms;
  while (!atomic_load_explicit(&control->exited, memory_order_acquire)) {
    (void)h2_pal_time_sleep_ms(runtime->time, 250u);
    (void)h2_pal_mutex_lock(runtime->sync, control->progress.mutex);
    const char *active_case = control->progress.active_case;
    (void)h2_pal_mutex_unlock(runtime->sync, control->progress.mutex);
    uint64_t now_ms = last_emit_ms;
    if (active_case != NULL &&
        h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) == H2_PAL_OK &&
        h2_pal_time_elapsed_ms(last_emit_ms, now_ms) >=
            control->progress.interval_ms) {
      progress_emit(&control->progress, H2_GIZCLAW_E2E_PROGRESS_RUNNING,
                    active_case, "RUNNING", NULL, H2_PAL_OK, false, true);
      last_emit_ms = now_ms;
    }
  }

  size_t runner_retained = 0u;
  const int runner_cleanup_rc =
      join_runner(control,
                  value_or_default(config->cleanup_timeout_ms,
                                   H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS),
                  &runner_retained);
  control->retained_resources += runner_retained;
  if (control->aggregate_cleanup_rc == H2_PAL_OK &&
      runner_cleanup_rc != H2_PAL_OK) {
    control->aggregate_cleanup_rc = runner_cleanup_rc;
  }

  control->progress.terminal = 0u;
  control->progress.passed = 0u;
  control->progress.failed = 0u;
  control->progress.errors = 0u;
  control->progress.blocked = 0u;
  control->progress.cancelled = 0u;
  for (size_t index = 0u; index < control->report.selected; ++index) {
    const h2_gizclaw_e2e_case_result_t *case_result =
        &control->report.cases[index];
    control->progress.terminal++;
    control->progress.passed += case_result->status == H2_GIZCLAW_E2E_CASE_PASS;
    control->progress.failed += case_result->status == H2_GIZCLAW_E2E_CASE_FAIL;
    control->progress.errors +=
        case_result->status == H2_GIZCLAW_E2E_CASE_ERROR;
    control->progress.blocked +=
        case_result->status == H2_GIZCLAW_E2E_CASE_BLOCKED;
    control->progress.cancelled +=
        case_result->status == H2_GIZCLAW_E2E_CASE_CANCELLED;
    progress_emit(&control->progress, H2_GIZCLAW_E2E_PROGRESS_CASE,
                  case_result->id,
                  h2_gizclaw_e2e_case_status_name(case_result->status),
                  case_result->blocked_by, case_result->rc, false, true);
  }

  progress_state_t *progress = &control->progress;
  progress_emit(progress, H2_GIZCLAW_E2E_PROGRESS_CLEANUP, NULL,
                control->aggregate_cleanup_rc == H2_PAL_OK ? "PASS" : "ERROR",
                NULL, control->aggregate_cleanup_rc, false, true);
  h2_gizclaw_e2e_report_cleanup(&control->report,
                                control->aggregate_cleanup_rc);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&control->report);
  copy_summary(&summary, control->runtime_profile_name, out_result);
  out_result->retained_resources = control->retained_resources;
  progress_emit(progress, H2_GIZCLAW_E2E_PROGRESS_SUMMARY, NULL, "SUMMARY",
                NULL, summary.first_failure_rc, summary.complete, true);

  progress->on_progress = NULL;
  progress->progress_user = NULL;
  if (runner_retained == 0u && control->retained_fixture == NULL) {
    h2_pal_mem_free(runtime->mem, control);
    atomic_flag_clear_explicit(&s_run_active, memory_order_release);
  } else {
    control->runtime = NULL;
    control->config = NULL;
    progress->runtime = NULL;
  }
  const int exit_code = h2_gizclaw_e2e_summary_exit_code(&summary);
  return exit_code == 0 ? H2_GIZCLAW_E2E_EXIT_PASS
                        : (exit_code == 1 ? H2_GIZCLAW_E2E_EXIT_CASE_FAILURE
                                          : H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
}
