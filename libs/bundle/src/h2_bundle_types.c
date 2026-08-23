#include "h2_bundle_types.h"

const char *h2_bundle_result_name(int result) {
    switch (result) {
    case H2_BUNDLE_OK:
        return "OK";
    case H2_BUNDLE_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case H2_BUNDLE_ERR_NO_MEMORY:
        return "NO_MEMORY";
    case H2_BUNDLE_ERR_NO_SPACE:
        return "NO_SPACE";
    case H2_BUNDLE_ERR_IO:
        return "IO";
    case H2_BUNDLE_ERR_FS:
        return "FS";
    case H2_BUNDLE_ERR_ZLIB:
        return "ZLIB";
    case H2_BUNDLE_ERR_TAR:
        return "TAR";
    case H2_BUNDLE_ERR_UNSAFE_PATH:
        return "UNSAFE_PATH";
    case H2_BUNDLE_ERR_UNSUPPORTED_ENTRY:
        return "UNSUPPORTED_ENTRY";
    case H2_BUNDLE_ERR_PIXA:
        return "PIXA";
    case H2_BUNDLE_ERR_MANIFEST:
        return "MANIFEST";
    case H2_BUNDLE_ERR_LAYOUT:
        return "LAYOUT";
    default:
        return "UNKNOWN";
    }
}
