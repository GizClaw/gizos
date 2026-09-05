#ifndef H2_GIZCLAW_E2E_TASK_NAMES_H
#define H2_GIZCLAW_E2E_TASK_NAMES_H

#define H2_GIZCLAW_E2E_RUNNER_TASK_NAME_VALUE "gizclaw/e2e/runner"
#define H2_GIZCLAW_E2E_WIFI_TASK_NAME_VALUE "gizclaw/e2e/wifi"
#define H2_GIZCLAW_E2E_LAUNCHER_TASK_NAME_VALUE "gizclaw/e2e/launcher"
#define H2_GIZCLAW_E2E_JOB_TASK_NAME_VALUE "gizclaw/e2e/job"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_gizclaw_e2e_runner_task_name[sizeof(
    H2_GIZCLAW_E2E_RUNNER_TASK_NAME_VALUE)];
extern const char
    h2_gizclaw_e2e_wifi_task_name[sizeof(H2_GIZCLAW_E2E_WIFI_TASK_NAME_VALUE)];
extern const char h2_gizclaw_e2e_launcher_task_name[sizeof(
    H2_GIZCLAW_E2E_LAUNCHER_TASK_NAME_VALUE)];
extern const char
    h2_gizclaw_e2e_job_task_name[sizeof(H2_GIZCLAW_E2E_JOB_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
