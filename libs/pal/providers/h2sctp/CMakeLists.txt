cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(h2sctp LANGUAGES C)
endif()

if(TARGET h2sctp)
  return()
endif()

set(H2_SCTP_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(DEFINED H2_REPOSITORY_ROOT)
  set(H2_SCTP_REPO_ROOT "${H2_REPOSITORY_ROOT}")
else()
  get_filename_component(H2_SCTP_REPO_ROOT "${H2_SCTP_ROOT}/../.." ABSOLUTE)
endif()

add_library(h2sctp STATIC
  "${H2_SCTP_ROOT}/src/h2_sctp.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_association.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_crc32c.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_reliability.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_stream.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_timer.c"
  "${H2_SCTP_ROOT}/src/h2_sctp_wire.c")
target_include_directories(h2sctp
  PUBLIC
    "${H2_SCTP_ROOT}/include"
    "${H2_SCTP_REPO_ROOT}/libs/pal/include"
  PRIVATE "${H2_SCTP_ROOT}/src")
target_compile_features(h2sctp PUBLIC c_std_11)
target_compile_options(h2sctp PRIVATE -Wall -Wextra -Werror)
