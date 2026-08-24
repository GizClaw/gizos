cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(h2libsrtp LANGUAGES C)
endif()

if(TARGET h2libsrtp)
  return()
endif()

set(H2_LIBSRTP_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(DEFINED H2_REPOSITORY_ROOT)
  set(H2_LIBSRTP_REPO_ROOT "${H2_REPOSITORY_ROOT}")
else()
  get_filename_component(
    H2_LIBSRTP_REPO_ROOT
    "${H2_LIBSRTP_ROOT}/../../../../../.."
    ABSOLUTE)
endif()
if(NOT DEFINED H2_LIBSRTP_UPSTREAM_ROOT OR H2_LIBSRTP_UPSTREAM_ROOT STREQUAL "")
  message(FATAL_ERROR
    "H2_LIBSRTP_UPSTREAM_ROOT must point to the verified Bazel vendor archive")
endif()

set(H2_LIBSRTP_UPSTREAM_SOURCES
  crypto/cipher/cipher.c
  crypto/cipher/cipher_test_cases.c
  crypto/cipher/null_cipher.c
  crypto/hash/auth.c
  crypto/hash/auth_test_cases.c
  crypto/hash/null_auth.c
  crypto/kernel/crypto_kernel.c
  crypto/kernel/err.c
  crypto/kernel/key.c
  crypto/math/datatypes.c
  crypto/replay/rdb.c
  crypto/replay/rdbx.c
  srtp/srtp.c)
list(TRANSFORM H2_LIBSRTP_UPSTREAM_SOURCES
  PREPEND "${H2_LIBSRTP_UPSTREAM_ROOT}/")

add_library(h2libsrtp STATIC
  "${H2_LIBSRTP_ROOT}/src/netinet/in.h"
  "${H2_LIBSRTP_ROOT}/src/h2_libsrtp.c"
  "${H2_LIBSRTP_ROOT}/src/h2_libsrtp_alloc.c"
  "${H2_LIBSRTP_ROOT}/src/h2_libsrtp_crypto.c"
  ${H2_LIBSRTP_UPSTREAM_SOURCES})

target_include_directories(h2libsrtp
  PUBLIC
    "${H2_LIBSRTP_ROOT}/include"
    "${H2_LIBSRTP_REPO_ROOT}/libs/pal/include"
  PRIVATE
    "${H2_LIBSRTP_ROOT}/src"
    "${H2_LIBSRTP_UPSTREAM_ROOT}/crypto/cipher"
    "${H2_LIBSRTP_UPSTREAM_ROOT}/crypto/hash"
    "${H2_LIBSRTP_UPSTREAM_ROOT}/crypto/include"
    "${H2_LIBSRTP_UPSTREAM_ROOT}/include")
target_compile_features(h2libsrtp PUBLIC c_std_11)
target_compile_definitions(h2libsrtp PRIVATE HAVE_CONFIG_H)
if(NOT UNIX AND NOT WIN32)
  target_compile_definitions(h2libsrtp PRIVATE H2_LIBSRTP_NO_SYSTEM_NETINET)
endif()
target_compile_options(h2libsrtp PRIVATE
  -Wall
  -Wextra
  -Werror
  -Wno-error=incompatible-pointer-types
  -Wno-error=format
  -fno-strict-aliasing)
