#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>
#include <string.h>

struct h2_pal_task {
    beken_thread_t thread;
    beken_semaphore_t done;
    h2_pal_task_entry_t entry;
    void *ctx;
};

static uint8_t bk_task_priority_for_name(const char *name) {
    if (name != NULL &&
        (strcmp(name, "cmd/h2test") == 0 ||
            strcmp(name, "cmd/apploader") == 0)) {
        return BEKEN_APPLICATION_PRIORITY;
    }
    return BEKEN_DEFAULT_WORKER_PRIORITY;
}

static const char *bk_task_name_for_name(const char *name) {
    if (name != NULL && strcmp(name, "cmd/h2test") == 0) {
        return "h2test";
    }
    if (name != NULL && strcmp(name, "cmd/apploader") == 0) {
        return "h2loader";
    }
    return name != NULL ? name : "h2_task";
}

static int bk_task_prefers_psram(const char *name) {
    return name != NULL &&
           (strcmp(name, "cmd/h2test") == 0 ||
               strcmp(name, "cmd/apploader") == 0 ||
               strcmp(name, "h2loader/appcmd") == 0 ||
               strcmp(name, "h2loader/return") == 0 ||
               strcmp(name, "bleikcp/kcp") == 0 ||
               strcmp(name, "bleikcp/server") == 0 ||
               strcmp(name, "ble_speed_p") == 0 ||
               strcmp(name, "ble_speed_c") == 0);
}

static void bk_task_trampoline(void *raw) {
    h2_pal_task_t *task = (h2_pal_task_t *)raw;
    task->entry(task->ctx);
    (void)rtos_set_semaphore(&task->done);
    rtos_delete_thread(NULL);
}

static int bk_task_start(void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;

    if (options == NULL || entry == NULL || out_task == NULL) {
        return -1;
    }

    h2_pal_task_t *task = (h2_pal_task_t *)os_malloc(sizeof(*task));
    if (task == NULL) {
        return -1;
    }
    os_memset(task, 0, sizeof(*task));
    if (rtos_init_semaphore(&task->done, 1) != kNoErr) {
        os_free(task);
        return -1;
    }
    task->entry = entry;
    task->ctx = ctx;

    uint32_t stack_size = options->min_stack_size > 0u ? (uint32_t)options->min_stack_size : 4096u;
    if (stack_size < 4096u) {
        stack_size = 4096u;
    }

    int ret;
    if (bk_task_prefers_psram(options->name)) {
        ret = rtos_create_psram_thread(&task->thread,
            bk_task_priority_for_name(options->name),
            bk_task_name_for_name(options->name),
            (beken_thread_function_t)bk_task_trampoline,
            stack_size,
            task);
    } else {
        ret = rtos_create_thread(&task->thread,
            bk_task_priority_for_name(options->name),
            bk_task_name_for_name(options->name),
            (beken_thread_function_t)bk_task_trampoline,
            stack_size,
            task);
    }
    if (ret != kNoErr) {
        printf("H2_PAL_TASK_ERROR op=create name=%s ret=%d stack=%lu\n",
            bk_task_name_for_name(options->name),
            ret,
            (unsigned long)stack_size);
        rtos_deinit_semaphore(&task->done);
        os_free(task);
        return -1;
    }
    if (bk_task_prefers_psram(options->name)) {
        printf("H2_PAL_TASK_READY name=%s stack=psram size=%lu\n",
            bk_task_name_for_name(options->name),
            (unsigned long)stack_size);
    }

    *out_task = task;
    return 0;
}

static int bk_task_join(void *user, h2_pal_task_t *task) {
    (void)user;

    if (task == NULL) {
        return -1;
    }
    int ret = rtos_get_semaphore(&task->done, BEKEN_WAIT_FOREVER);
    if (ret != kNoErr) {
        printf("H2_PAL_TASK_ERROR op=join ret=%d\n", ret);
        return -1;
    }
    rtos_deinit_semaphore(&task->done);
    os_free(task);
    return 0;
}

const h2_pal_task_api_t *h2_bk_platform_task_api(void) {
    static const h2_pal_task_vtable_t vtable = {
        .start = bk_task_start,
        .join = bk_task_join,
    };
    static const h2_pal_task_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
