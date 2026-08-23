#ifndef H2_BUNDLE_ARCHIVE_H
#define H2_BUNDLE_ARCHIVE_H

#include "h2_bundle_installer.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_bundle_archive_install_zlib_tar(h2_bundle_installer_t *installer, const h2_bundle_install_options_t *options);

#ifdef __cplusplus
}
#endif

#endif
