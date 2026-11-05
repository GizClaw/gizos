#include "h2_bk_platform_storage.h"
#include "h2/pal/h2_pal_unsupported.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
  static const h2_bk_sd_mount_t mounts[] = {
      {.path = "/data", .native_path = "1:/data"},
  };
  const h2_bk_sd_fs_config_t config = {
      .drive = "1:",
      .root = "1:/",
      .mounts = mounts,
      .mount_count = 1u,
  };
  h2_pal_fs_api_t fs = {0};
  assert(h2_bk_platform_sd_fs_init(NULL, &fs) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_platform_sd_fs_init(&config, NULL) == H2_PAL_ERR_INVALID_ARG);
  for (unsigned i = 0u; i < 2u; ++i) {
    assert(h2_bk_platform_sd_fs_init(&config, &fs) == H2_PAL_ERR_UNSUPPORTED);
    assert(fs.user == h2_pal_unsupported_fs_api()->user &&
           fs.vtable == h2_pal_unsupported_fs_api()->vtable);
    assert(h2_pal_fs_prepare(&fs, "/data") == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_fs_clear(&fs, "/data") == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_bk_platform_sd_fs_mount(NULL, "/data") == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_bk_platform_sd_fs_clear(NULL, "/data") == H2_PAL_ERR_UNSUPPORTED);
    h2_bk_platform_sd_fs_prepare();
    h2_bk_platform_sd_fs_release();
  }
  assert(h2_bk_platform_sd_fs_mount(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_platform_sd_fs_clear(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
