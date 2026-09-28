cmake_minimum_required(VERSION 3.19)
project(bk_dtls_contract_test C)

# Compile the SDK's actual mbedTLS sources out of tree; never edit the SDK.
if(NOT DEFINED BK7258_PATH OR NOT DEFINED H2_REPO_ROOT)
    message(FATAL_ERROR "BK7258_PATH and H2_REPO_ROOT are required")
endif()
set(MBEDTLS_DIR "${BK7258_PATH}/ap/components/psa_mbedtls/mbedtls")
set(GEN_FILES OFF)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON)
set(CMAKE_C_STANDARD 11)
add_compile_options(-include "${CMAKE_CURRENT_SOURCE_DIR}/host_log.h")
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/host_config.h")

# The SDK omits upstream's top-level generator assets. Its library directory
# supports standalone inclusion with the four pre-generated sources below.
function(link_to_source base_name)
    file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${base_name}"
        "${CMAKE_CURRENT_BINARY_DIR}/${base_name}" SYMBOLIC)
endfunction()
add_subdirectory("${MBEDTLS_DIR}/library" mbedtls)

add_executable(bk_dtls_contract_test
    test_dtls.c
    "${H2_REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_dtls.c")
target_include_directories(bk_dtls_contract_test PRIVATE
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include")
target_compile_options(bk_dtls_contract_test PRIVATE -UNDEBUG)
target_link_libraries(bk_dtls_contract_test mbedtls mbedx509 mbedcrypto)
enable_testing()
add_test(NAME bk_dtls_contract COMMAND bk_dtls_contract_test)
