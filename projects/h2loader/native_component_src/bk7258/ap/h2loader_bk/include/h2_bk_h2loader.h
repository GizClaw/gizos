#ifndef H2_BK_H2LOADER_H
#define H2_BK_H2LOADER_H

#include "h2_bundle_installer.h"
#include "h2_loader_command.h"
#include "h2_loader_package.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BK_H2LOADER_APP_ENTRY_PATH "app/bk/app_ab_crc.rbl"
#define H2_BK_H2LOADER_PRIMARY_PARTITION_ID 1u
#define H2_BK_H2LOADER_APP_PARTITION_ID 2u
#define H2_BK_H2LOADER_FATFS_DRIVE "1:"
#define H2_BK_H2LOADER_SD_ROOT "1:/h2loader"
#define H2_BK_H2LOADER_SD_DL_ROOT "1:/h2loader/dl"
#define H2_BK_H2LOADER_SD_DATA_ROOT "1:/h2loader/data"
#define H2_BK_H2LOADER_COREDUMP_ADDR 0x7a0000u
#define H2_BK_H2LOADER_COREDUMP_SIZE (360u * 1024u)
#define H2_BK_H2LOADER_APP_COMMAND_STACK_SIZE 8192u
#define H2_BK_H2LOADER_LOADER_COMMAND_STACK_SIZE 49152u

int h2_bk_h2loader_sd_fs_init(h2_pal_fs_api_t *fs);
void h2_bk_h2loader_prepare_sd_storage(void);
void h2_bk_h2loader_release_sd_storage(void);
int h2_bk_h2loader_mount_file_point(void *user, const char *path);
int h2_bk_h2loader_clear_data(void *user, const char *path);
const h2_bundle_app_writer_t *h2_bk_h2loader_ota_app_writer(void);
const h2_loader_image_reader_api_t *h2_bk_h2loader_image_reader(void);
const h2_loader_image_writer_api_t *h2_bk_h2loader_image_writer(void);
int h2_bk_h2loader_managed_app_image_size(uint64_t *out_size);
int h2_bk_h2loader_commit_staged_app_boot(void);
const h2_pal_power_api_t *h2_bk_h2loader_power_api(void);
int h2_bk_h2loader_start_app_iostreamikcp(h2_runtime_t *runtime,
                                          const char *active_name);
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities);
int h2_bk_h2loader_stop_app_iostreamikcp(void);
int h2_bk_h2loader_start_loader_iostreamikcp(
    h2_runtime_t *runtime, h2_loader_command_t *command,
    const h2_loader_command_config_t *command_config);
int h2_bk_h2loader_stop_loader_iostreamikcp(void);
int h2_bk_h2loader_start_app_ble(h2_runtime_t *runtime,
                                 const char *active_name);
int h2_bk_h2loader_start_app_ble_with_capabilities(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities);
int h2_bk_h2loader_start_app_ble_extended(h2_runtime_t *runtime,
                                          const char *active_name);
int h2_bk_h2loader_start_app_ble_extended_with_capabilities(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities);
int h2_bk_h2loader_pause_app_ble_advertising(void);
int h2_bk_h2loader_resume_app_ble_advertising(void);
int h2_bk_h2loader_advertise_app_ble_service(
    const h2_pal_ble_uuid_t *service_uuid);
int h2_bk_h2loader_app_operation_lock(void);
void h2_bk_h2loader_app_operation_unlock(void);
int h2_bk_h2loader_reboot_to_loader(void);
int h2_bk_h2loader_confirm_active_loader(void *user);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t *runtime);
int h2_bk_h2loader_prepare_pending_app_restart(void);
void h2_bk_h2loader_abort_for_crash_test(void);

#ifdef __cplusplus
}
#endif

#endif
