#ifndef H2_ESP_PLATFORM_ENTRY_TASK_H
#define H2_ESP_PLATFORM_ENTRY_TASK_H

#include "core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_esp_platform_entry_task_fn)(void* user);

/** Start a detached native entry using the installed generated target policy.
 * Call from serialized startup task context, never from an ISR. name must be
 * nonempty and entry must be non-NULL. name is borrowed until SDK creation
 * returns; user and after_entry_user remain borrowed until their callbacks
 * return. The entry owns shutdown of its Runtime and all asynchronous borrows.
 * The optional after_entry callback runs on the same task after entry returns,
 * before self-deletion, and must release only resources it owns. Neither
 * callback runs on failure. Context storage is freed before entry runs.
 * Policy resolution seals Task configuration, including on lookup failure.
 * The policy selects priority, affinity, minimum stack bytes and memory region;
 * detached stacks use SDK allocation, independently of the PAL joinable-task
 * stack allocator. Returns INVALID_ARG, INVALID_STATE, NOT_FOUND, TASK,
 * NO_MEMORY, or OK. Success transfers execution to the detached task; there is
 * no join handle. */
h2_pal_result_t h2_esp_platform_start_entry_task(
    const char* name, h2_esp_platform_entry_task_fn entry, void* user,
    h2_esp_platform_entry_task_fn after_entry, void* after_entry_user);

#ifdef __cplusplus
}
#endif

#endif
