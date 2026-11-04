#ifndef H2_BK7258_BOARD_RUNTIME_TEST_SDK_H
#define H2_BK7258_BOARD_RUNTIME_TEST_SDK_H

#include "native_component_src/bk7258/ap/h2_pal_storage/tests/h2_bk_sd_fs_test_sdk.h"

typedef uint32_t FSIZE_t;
FRESULT f_lseek(FIL* file, FSIZE_t offset);
#define f_tell(file) ((FSIZE_t)(file)->marker)

#endif
