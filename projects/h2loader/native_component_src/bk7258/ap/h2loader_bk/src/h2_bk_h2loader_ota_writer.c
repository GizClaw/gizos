#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"

#include "bk_private/bk_ota_private.h"
#include "common/bk_err.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "driver/wdt.h"
#include "h2_loader_boot.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "modules/ota.h"
#include "os/os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void bk_wdt_force_feed(void);
extern part_flag update_part_flag;
extern uint32_t ota_calc_hash(uint32_t hash, const void *buf, size_t len);
extern int32_t ota_read_partition(
    const bk_logic_partition_t *part,
    uint32_t addr,
    uint8_t *buf,
    size_t size,
    uint32_t offset);

#define H2_BK_OTA_PROGRESS_STEP (256u * 1024u)
#define H2_BK_OTA_FLASH_SECTOR_SIZE 4096u
#define H2_BK_OTA_VERIFY_CHUNK_SIZE 4096u
#define H2_BK_OTA_CRC_BLOCK_DATA_SIZE 32u
#define H2_BK_OTA_CRC_BLOCK_SIZE 34u
#define H2_BK_OTA_TEMP_EXEC_OFFSET 4u
#define H2_BK_OTA_CONFIRM_OFFSET 8u
#define H2_BK_OTA_PENDING_CONFIRM 1u
#define H2_BK_OTA_RBL_FOOTER_PHYSICAL_OFFSET \
    ((RBL_HEAD_POS * H2_BK_OTA_CRC_BLOCK_SIZE) / H2_BK_OTA_CRC_BLOCK_DATA_SIZE)
#define H2_BK_CRASH_EVIDENCE_SIZE 32u

static const bk_logic_partition_t *s_partition;
static flash_protect_type_t s_protect_type;
static uint32_t s_erased;
static uint32_t s_next_erase_progress;
static uint32_t s_received;
static uint32_t s_total;
static uint32_t s_next_progress;
static int s_flash_open;
static int s_staged_app_ready;
static uint8_t s_verify_buffer[H2_BK_OTA_VERIFY_CHUNK_SIZE];
static bk_logic_partition_t s_primary_window_partition;

static const bk_logic_partition_t *image_partition(uint32_t partition_id);

static int map_bk_result(int rc) {
    return rc == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static void feed_watchdogs(void) {
    (void)bk_wdt_feed();
    bk_wdt_force_feed();
}

static void log_progress(const char *stage, uint32_t offset) {
    char line[128];

    snprintf(
        line,
        sizeof(line),
        "H2_BK_OTA_WRITER stage=%s offset=%lu total=%lu\r\n",
        stage,
        (unsigned long)offset,
        (unsigned long)s_total);
    os_printf("%s", line);
}

static int write_execution_flags(uint8_t final_exec, uint8_t temp_exec, uint8_t confirm) {
    const bk_logic_partition_t *control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
    flash_protect_type_t protect;
    uint8_t flags[12];
    int rc;
    char line[128];

    if (control == NULL) {
        return BK_FAIL;
    }
    memset(flags, 0xff, sizeof(flags));
    memcpy(&flags[0], &final_exec, sizeof(final_exec));
    memcpy(&flags[H2_BK_OTA_TEMP_EXEC_OFFSET], &temp_exec, sizeof(temp_exec));
    memcpy(&flags[H2_BK_OTA_CONFIRM_OFFSET], &confirm, sizeof(confirm));

    protect = bk_flash_get_protect_type();
    rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    if (rc == BK_OK) {
        rc = bk_flash_erase_sector(control->partition_start_addr);
    }
    if (rc == BK_OK) {
        rc = bk_flash_write_bytes(control->partition_start_addr, flags, sizeof(flags));
    }
    (void)bk_flash_set_protect_type(protect);

    snprintf(
        line,
        sizeof(line),
        "H2_BK_OTA_WRITER stage=write_flags rc=%d final=%u temp=%u confirm=%u\r\n",
        rc,
        (unsigned int)final_exec,
        (unsigned int)temp_exec,
        (unsigned int)confirm);
    os_printf("%s", line);
    return rc;
}

static void close_flash_writer(void) {
    if (s_flash_open) {
        (void)bk_flash_set_protect_type(s_protect_type);
        s_flash_open = 0;
    }
    s_partition = NULL;
    s_erased = 0u;
    s_next_erase_progress = 0u;
    s_received = 0u;
    s_total = 0u;
    s_next_progress = 0u;
}

static int erase_for_range(uint32_t end_offset) {
    int rc;

    while (s_erased < end_offset) {
        if (s_erased >= s_next_erase_progress) {
            log_progress("erase", s_erased);
            while (s_next_erase_progress <= s_erased &&
                UINT32_MAX - s_next_erase_progress >= H2_BK_OTA_PROGRESS_STEP) {
                s_next_erase_progress += H2_BK_OTA_PROGRESS_STEP;
            }
        }
        rc = bk_flash_erase_sector(s_partition->partition_start_addr + s_erased);
        feed_watchdogs();
        if (rc != BK_OK) {
            return rc;
        }
        if (UINT32_MAX - s_erased < H2_BK_OTA_FLASH_SECTOR_SIZE) {
            return BK_FAIL;
        }
        s_erased += H2_BK_OTA_FLASH_SECTOR_SIZE;
    }
    return BK_OK;
}

static int verify_staged_rbl(void) {
    struct ota_rbl_head header;
    uint32_t hash = RT_OTA_HASH_FNV_SEED;
    uint32_t logical_capacity;
    uint32_t offset = 0u;
    uint32_t next_progress = 0u;
    int32_t read_rc;

    if (!s_flash_open || s_partition == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (s_total < H2_BK_OTA_RBL_FOOTER_PHYSICAL_OFFSET) {
        return H2_PAL_ERR_IO;
    }
    logical_capacity =
        (s_total / H2_BK_OTA_CRC_BLOCK_SIZE) * H2_BK_OTA_CRC_BLOCK_DATA_SIZE;
    memset(&header, 0, sizeof(header));
    if (ota_get_rbl_head(s_partition, &header, s_total) != BK_OK ||
        memcmp(header.magic, "RBL", 3u) != 0 ||
        header.size_raw == 0u ||
        header.size_raw > logical_capacity) {
        return H2_PAL_ERR_IO;
    }

    while (offset < header.size_raw) {
        uint32_t take = header.size_raw - offset;

        if (take > sizeof(s_verify_buffer)) {
            take = sizeof(s_verify_buffer);
        }
        if (offset >= next_progress) {
            char line[128];

            snprintf(
                line,
                sizeof(line),
                "H2_BK_OTA_WRITER stage=verify offset=%lu total=%lu\r\n",
                (unsigned long)offset,
                (unsigned long)header.size_raw);
            os_printf("%s", line);
            while (next_progress <= offset && UINT32_MAX - next_progress >= H2_BK_OTA_PROGRESS_STEP) {
                next_progress += H2_BK_OTA_PROGRESS_STEP;
            }
        }
        read_rc = ota_read_partition(s_partition, offset, s_verify_buffer, take, 0u);
        if (read_rc != (int32_t)take) {
            return H2_PAL_ERR_IO;
        }
        hash = ota_calc_hash(hash, s_verify_buffer, take);
        offset += take;
        feed_watchdogs();
    }
    if (hash != header.hash) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static int ota_writer_begin_partition(uint32_t partition_id, uint64_t image_size) {
    const bk_logic_partition_t *target;
    char line[128];

    close_flash_writer();
    os_printf("H2_BK_OTA_WRITER stage=begin_enter\r\n");
    feed_watchdogs();
    if (image_size == 0u || image_size > UINT32_MAX ||
        (partition_id != H2_BK_H2LOADER_PRIMARY_PARTITION_ID &&
            partition_id != H2_BK_H2LOADER_APP_PARTITION_ID)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID &&
            bk_ota_get_current_partition() == EXEX_A_PART) ||
        (partition_id == H2_BK_H2LOADER_APP_PARTITION_ID &&
            bk_ota_get_current_partition() == EXEC_B_PART)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    target = image_partition(partition_id);
    if (target == NULL || image_size > target->partition_length) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_protect_type = bk_flash_get_protect_type();
    if (bk_flash_set_protect_type(FLASH_PROTECT_NONE) != BK_OK) {
        return H2_PAL_ERR_IO;
    }
    s_flash_open = 1;
    s_partition = target;
    s_erased = 0u;
    s_next_erase_progress = 0u;
    s_received = 0u;
    s_total = (uint32_t)image_size;
    s_next_progress = 0u;
    s_staged_app_ready = 0;
    update_part_flag = partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID ?
        UPDATE_A_PART : UPDATE_B_PART;
    snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=begin bytes=%lu target=%lu\r\n",
        (unsigned long)s_total, (unsigned long)partition_id);
    os_printf("%s", line);
    return H2_PAL_OK;
}

static int ota_writer_begin(void *user, const h2_bundle_entry_t *entry) {
    (void)user;
    if (entry == NULL || entry->path == NULL ||
        strcmp(entry->path, H2_BK_H2LOADER_APP_ENTRY_PATH) != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return ota_writer_begin_partition(H2_BK_H2LOADER_APP_PARTITION_ID, entry->size);
}

static int ota_writer_write(void *user, const h2_bundle_entry_t *entry, const void *data, size_t len) {
    uint32_t write_len;
    uint32_t write_end;
    int rc;

    (void)user;
    (void)entry;
    if ((data == NULL && len != 0u) || len > UINT32_MAX || s_received > s_total ||
        (uint32_t)len > s_total - s_received) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    write_len = (uint32_t)len;
    feed_watchdogs();
    if (!s_flash_open || s_partition == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    write_end = s_received + write_len;
    rc = erase_for_range(write_end);
    if (rc != BK_OK) {
        close_flash_writer();
        s_staged_app_ready = 0;
        return map_bk_result(rc);
    }
    if (s_received >= s_next_progress) {
        log_progress("write", s_received);
        while (s_next_progress <= s_received && UINT32_MAX - s_next_progress >= H2_BK_OTA_PROGRESS_STEP) {
            s_next_progress += H2_BK_OTA_PROGRESS_STEP;
        }
    }
    rc = bk_flash_write_bytes(s_partition->partition_start_addr + s_received, data, write_len);
    feed_watchdogs();
    if (write_end == s_total) {
        char line[96];
        snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=final_write rc=%d\r\n", rc);
        os_printf("%s", line);
    }
    if (rc != BK_OK) {
        close_flash_writer();
        s_staged_app_ready = 0;
        return map_bk_result(rc);
    }
    s_received = write_end;
    return H2_PAL_OK;
}

static int ota_writer_end(void *user, const h2_bundle_entry_t *entry) {
    int rc;

    (void)user;
    if (entry == NULL || !s_flash_open || s_partition == NULL || s_received != s_total) {
        close_flash_writer();
        s_staged_app_ready = 0;
        return H2_PAL_ERR_IO;
    }
    {
        char line[96];
        snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=end bytes=%lu\r\n", (unsigned long)s_received);
        os_printf("%s", line);
    }
    feed_watchdogs();
    rc = verify_staged_rbl();
    if (rc != H2_PAL_OK) {
        close_flash_writer();
        s_staged_app_ready = 0;
        return rc;
    }
    close_flash_writer();
    s_staged_app_ready = 1;
    return H2_PAL_OK;
}

const h2_bundle_app_writer_t *h2_bk_h2loader_ota_app_writer(void) {
    static const h2_bundle_app_writer_t writer = {
        .user = NULL,
        .begin = ota_writer_begin,
        .write = ota_writer_write,
        .end = ota_writer_end,
    };
    return &writer;
}

int h2_bk_h2loader_commit_staged_app_boot(void) {
    int rc;

    if (!s_staged_app_ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    feed_watchdogs();
    rc = map_bk_result(bk_ota_update_partition_flag(BK_OK));
    feed_watchdogs();
    {
        char line[96];
        snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=commit rc=%d\r\n", rc);
        os_printf("%s", line);
    }
    if (rc == H2_PAL_OK) {
        s_staged_app_ready = 0;
    }
    return rc;
}

static const bk_logic_partition_t *image_partition(uint32_t partition_id) {
    if (partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID) {
        const bk_logic_partition_t *primary_cp =
            bk_flash_partition_get_info(BK_PARTITION_APPLICATION);
        const bk_logic_partition_t *primary_ap =
            bk_flash_partition_get_info(BK_PARTITION_APPLICATION1);
        const bk_logic_partition_t *trial =
            bk_flash_partition_get_info(BK_PARTITION_S_APP);
        uint32_t primary_length;

        if (primary_cp == NULL || primary_ap == NULL || trial == NULL ||
            UINT32_MAX - primary_cp->partition_length < primary_ap->partition_length ||
            UINT32_MAX - primary_cp->partition_start_addr < primary_cp->partition_length ||
            UINT32_MAX - primary_ap->partition_start_addr < primary_ap->partition_length) {
            return NULL;
        }
        primary_length = primary_cp->partition_length + primary_ap->partition_length;
        if (primary_cp->partition_start_addr + primary_cp->partition_length !=
                primary_ap->partition_start_addr ||
            primary_ap->partition_start_addr + primary_ap->partition_length !=
                trial->partition_start_addr ||
            primary_length != trial->partition_length) {
            return NULL;
        }
        s_primary_window_partition = *primary_cp;
        s_primary_window_partition.partition_description = "h2loader_primary";
        s_primary_window_partition.partition_length = primary_length;
        return &s_primary_window_partition;
    }
    if (partition_id == H2_BK_H2LOADER_APP_PARTITION_ID) {
        return bk_flash_partition_get_info(BK_PARTITION_S_APP);
    }
    return NULL;
}

static int image_get_capacity(void *user, uint32_t partition_id, uint64_t *out_capacity) {
    const bk_logic_partition_t *partition;
    (void)user;
    if (out_capacity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = image_partition(partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_capacity = partition->partition_length;
    return H2_PAL_OK;
}

static int image_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    const bk_logic_partition_t *partition;
    (void)user;
    partition = image_partition(partition_id);
    if (partition == NULL || (data == NULL && len != 0u) ||
        offset > partition->partition_length || len > partition->partition_length - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return bk_flash_read_bytes(partition->partition_start_addr + (uint32_t)offset, data, len) == BK_OK ?
        H2_PAL_OK : H2_PAL_ERR_IO;
}

static int image_writer_begin(
    void *user,
    uint32_t partition_id,
    const h2_loader_image_identity_t *identity) {
    (void)user;
    if (identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return ota_writer_begin_partition(partition_id, identity->image_size);
}

static int image_writer_write(void *user, const void *data, size_t len) {
    return ota_writer_write(user, NULL, data, len);
}

static int image_writer_finish(void *user, const h2_loader_image_identity_t *identity) {
    h2_bundle_entry_t entry;
    (void)identity;
    memset(&entry, 0, sizeof(entry));
    return ota_writer_end(user, &entry);
}

static void image_writer_abort(void *user) {
    (void)user;
    close_flash_writer();
    s_staged_app_ready = 0;
}

const h2_loader_image_reader_api_t *h2_bk_h2loader_image_reader(void) {
    static const h2_loader_image_reader_vtable_t vtable = {
        .get_capacity = image_get_capacity,
        .read = image_read,
    };
    static const h2_loader_image_reader_api_t api = {.vtable = &vtable};
    return &api;
}

const h2_loader_image_writer_api_t *h2_bk_h2loader_image_writer(void) {
    static const h2_loader_image_writer_vtable_t vtable = {
        .get_capacity = image_get_capacity,
        .begin = image_writer_begin,
        .write = image_writer_write,
        .finish = image_writer_finish,
        .abort = image_writer_abort,
    };
    static const h2_loader_image_writer_api_t api = {.vtable = &vtable};
    return &api;
}

int h2_bk_h2loader_confirm_active_loader(void *user) {
    (void)user;
#if CONFIG_OTA_POSITION_INDEPENDENT_AB
    bk_ota_double_check_for_execution();
#else
    uint8_t current = bk_ota_get_current_partition();
    if (current == EXEX_A_PART) {
        bk_ota_confirm_update_partition(CONFIRM_EXEC_A);
    } else if (current == EXEC_B_PART) {
        bk_ota_confirm_update_partition(CONFIRM_EXEC_B);
    } else {
        return H2_PAL_ERR_INVALID_STATE;
    }
#endif
    return H2_PAL_OK;
}

int h2_bk_h2loader_confirm_current_app(h2_runtime_t *runtime) {
    const bk_logic_partition_t *partition;
    h2_loader_image_identity_t identity = {0};
    h2_pal_firmware_info_t firmware_info;
    uint8_t confirm_flag = 0xffu;
    uint8_t expected_flag;
    uint8_t expected_exec;
    int rc;

    if (runtime == NULL || runtime->pref == NULL || runtime->mem == NULL ||
        runtime->fs == NULL || runtime->firmware_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
#if CONFIG_OTA_POSITION_INDEPENDENT_AB
    expected_exec = bk_ota_get_current_partition() == EXEX_A_PART ? EXEX_A_PART : EXEC_B_PART;
    expected_flag = expected_exec == EXEX_A_PART ? CONFIRM_EXEC_A : CONFIRM_EXEC_B;
    bk_ota_double_check_for_execution();
#else
    expected_exec = EXEC_B_PART;
    expected_flag = CONFIRM_EXEC_B;
    bk_ota_confirm_update_partition(CONFIRM_EXEC_B);
#endif
    partition = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
    if (partition == NULL ||
        bk_flash_read_bytes(
            partition->partition_start_addr + 8u,
            &confirm_flag,
            sizeof(confirm_flag)) != BK_OK ||
        confirm_flag != expected_flag) {
        rc = write_execution_flags(expected_exec, expected_exec, expected_flag);
        if (rc != BK_OK) {
            return H2_PAL_ERR_IO;
        }
    }
    rc = h2_pal_firmware_info_get_current(
        runtime->firmware_info, &firmware_info);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_bk_h2loader_current_app_identity(
        runtime, firmware_info.version, &identity);
    if (rc != H2_PAL_OK) return rc;
    return h2_loader_finalize_active_app(
        runtime->pref, runtime->mem, runtime->fs,
        H2_LOADER_DEFAULT_PACKAGE_PATH, &identity,
        H2_BK_H2LOADER_APP_PARTITION_ID,
        H2_BK_H2LOADER_APP_PARTITION_ID);
}

int h2_bk_h2loader_prepare_pending_app_restart(void) {
    if (bk_ota_get_current_partition() != EXEC_B_PART) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return map_bk_result(
        write_execution_flags(EXEC_B_PART, EXEC_B_PART, H2_BK_OTA_PENDING_CONFIRM));
}

int h2_bk_h2loader_select_confirmed_boot_partition(uint32_t partition_id) {
    switch (partition_id) {
    case H2_BK_H2LOADER_PRIMARY_PARTITION_ID:
        return map_bk_result(
            write_execution_flags(EXEX_A_PART, EXEX_A_PART, CONFIRM_EXEC_A));
    case H2_BK_H2LOADER_APP_PARTITION_ID:
        return map_bk_result(
            write_execution_flags(EXEC_B_PART, EXEC_B_PART, CONFIRM_EXEC_B));
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
}

int h2_bk_h2loader_prepare_pending_app_rollback(void) {
    if (bk_ota_get_current_partition() != EXEC_B_PART) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_bk_h2loader_select_confirmed_boot_partition(
        H2_BK_H2LOADER_PRIMARY_PARTITION_ID);
}

void h2_bk_h2loader_abort_for_crash_test(void) {
    static const uint8_t evidence[] = {
        H2_BK_CRASH_EVIDENCE_SIZE, 0u, 0u, 0u,
        'H', '2', 'B', 'K', 'C', 'O', 'R', 'E',
        'c', 'r', 'a', 's', 'h', '-', 'b', 'e', 'f', 'o', 'r', 'e', '-',
        'c', 'o', 'n', 'f', 'i', 'r', 'm',
    };
    _Static_assert(sizeof(evidence) == H2_BK_CRASH_EVIDENCE_SIZE, "unexpected BK crash evidence size");
    flash_protect_type_t protect = bk_flash_get_protect_type();
    (void)bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    (void)bk_flash_erase_sector(H2_BK_H2LOADER_COREDUMP_ADDR);
    (void)bk_flash_write_bytes(
        H2_BK_H2LOADER_COREDUMP_ADDR,
        evidence,
        sizeof(evidence));
    (void)bk_flash_set_protect_type(protect);

    volatile int *p = (volatile int *)0;
    *p = 1;
}
