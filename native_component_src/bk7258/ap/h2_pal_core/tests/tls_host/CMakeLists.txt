cmake_minimum_required(VERSION 3.19)
project(bk_sha384_certificate_test C)
if(NOT DEFINED BK7258_PATH OR NOT DEFINED H2_REPO_ROOT)
    message(FATAL_ERROR "BK7258_PATH and H2_REPO_ROOT are required")
endif()
set(MBEDTLS_DIR "${BK7258_PATH}/ap/components/psa_mbedtls/mbedtls")
set(GEN_FILES OFF)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON)
set(CMAKE_C_STANDARD 11)

# Use the production PUBLIC feature block, including its transitive consumer
# configuration. The negative control removes only SHA-384 from that block.
file(READ "${H2_REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/CMakeLists.txt" production)
string(REGEX MATCH "target_compile_definitions\\(\\$\\{H2_BK_PSA_MBEDTLS_LIB\\} PUBLIC[^)]*\\)" feature_block "${production}")
if(NOT feature_block)
    message(FATAL_ERROR "Production PSA public feature block was not found")
endif()
string(REGEX REPLACE "#[^\n]*" "" feature_block "${feature_block}")
string(REGEX MATCHALL "MBEDTLS_[A-Z0-9_]+" features "${feature_block}")
option(DISABLE_SHA384 "Run the missing-feature negative control" OFF)
if(DISABLE_SHA384)
    list(REMOVE_ITEM features MBEDTLS_SHA384_C)
endif()
add_compile_definitions(${features})

# Minimal SDK OS include shims and host platform hooks do not change the
# certificate algorithms, public digest selection or verifier profile.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host_sdk/os")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/host_sdk/os/os.h" "#include <stddef.h>\n#include <stdio.h>\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/host_sdk/os/mem.h" "#include <stdlib.h>\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/host_sdk/sdkconfig.h" "/* Minimal SDK profile. */\n")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/host_sdk")
add_compile_definitions(
    MBEDTLS_CONFIG_FILE="${BK7258_PATH}/ap/components/psa_mbedtls/mbedtls_port/configs/mbedtls_psa_crypto_config.h"
    MBEDTLS_USER_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/host_config.h")
function(link_to_source base_name)
    file(CREATE_LINK "${MBEDTLS_DIR}/library/${base_name}"
        "${CMAKE_CURRENT_BINARY_DIR}/${base_name}" SYMBOLIC)
endfunction()
add_subdirectory("${MBEDTLS_DIR}/library" mbedtls)
add_executable(bk_sha384_certificate_test test_sha384.c)
target_compile_options(bk_sha384_certificate_test PRIVATE -UNDEBUG)
target_link_libraries(bk_sha384_certificate_test mbedx509 mbedcrypto)
enable_testing()
add_test(NAME bk_sha384_certificate COMMAND bk_sha384_certificate_test)
