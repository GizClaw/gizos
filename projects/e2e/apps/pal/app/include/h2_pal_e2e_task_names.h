#ifndef H2_PAL_E2E_TASK_NAMES_H
#define H2_PAL_E2E_TASK_NAMES_H

#define H2_PAL_E2E_CORE_TASK_NAME_VALUE "pal/e2e/core"
#define H2_PAL_E2E_RUNNER_TASK_NAME_VALUE "pal/e2e/runner"
#define H2_PAL_E2E_QUEUE_TASK_NAME_VALUE "pal/e2e/queue"
#define H2_PAL_E2E_CONDITION_TASK_NAME_VALUE "pal/e2e/condition"
#define H2_PAL_E2E_CONSUMER_TASK_NAME_VALUE "pal/e2e/consumer"
#define H2_PAL_E2E_PRODUCER_TASK_NAME_VALUE "pal/e2e/producer"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_pal_e2e_core_task_name[sizeof(H2_PAL_E2E_CORE_TASK_NAME_VALUE)];
extern const char
    h2_pal_e2e_runner_task_name[sizeof(H2_PAL_E2E_RUNNER_TASK_NAME_VALUE)];
extern const char
    h2_pal_e2e_queue_task_name[sizeof(H2_PAL_E2E_QUEUE_TASK_NAME_VALUE)];
extern const char h2_pal_e2e_condition_task_name[sizeof(
    H2_PAL_E2E_CONDITION_TASK_NAME_VALUE)];
extern const char
    h2_pal_e2e_consumer_task_name[sizeof(H2_PAL_E2E_CONSUMER_TASK_NAME_VALUE)];
extern const char
    h2_pal_e2e_producer_task_name[sizeof(H2_PAL_E2E_PRODUCER_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
