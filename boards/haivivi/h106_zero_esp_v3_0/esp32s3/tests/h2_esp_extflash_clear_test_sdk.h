#ifndef H2_ESP_EXTFLASH_CLEAR_TEST_SDK_H
#define H2_ESP_EXTFLASH_CLEAR_TEST_SDK_H
#include "os/h2_pal_fs.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#define H2_ESP_BOARD_FS_BASE_PATH "/fake"
#define ESP_LOGE(...) ((void)0)
DIR* test_opendir(const char*);
struct dirent* test_readdir(DIR*);
int test_closedir(DIR*);
int test_stat(const char*, struct stat*);
int test_unlink(const char*);
int test_rmdir(const char*);
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#define stat(...) test_stat(__VA_ARGS__)
#define unlink test_unlink
#define rmdir test_rmdir
int h2_esp_board_extflash_mount_data(void);
int test_ensure_directory(const char*);
int test_log_filesystem_usage(const char*);
#define ensure_directory test_ensure_directory
#define log_filesystem_usage test_log_filesystem_usage
#endif
