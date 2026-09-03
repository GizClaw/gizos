cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(h2peer LANGUAGES C)
endif()

if(TARGET h2peer)
  return()
endif()

set(H2PEER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(DEFINED H2_REPOSITORY_ROOT)
  set(H2PEER_REPO_ROOT "${H2_REPOSITORY_ROOT}")
else()
  get_filename_component(H2PEER_REPO_ROOT "${H2PEER_ROOT}/../../../.." ABSOLUTE)
endif()

if(NOT TARGET h2libsrtp)
  add_subdirectory(
    "${H2PEER_ROOT}/internal/libsrtp"
    "${CMAKE_CURRENT_BINARY_DIR}/h2libsrtp")
endif()

set(H2PEER_PORTABLE_SOURCES
  "${H2PEER_ROOT}/src/providers/portable/address.c"
  "${H2PEER_ROOT}/src/providers/portable/agent.c"
  "${H2PEER_ROOT}/src/providers/portable/base64.c"
  "${H2PEER_ROOT}/src/providers/portable/dtls_srtp.c"
  "${H2PEER_ROOT}/src/providers/portable/ice.c"
  "${H2PEER_ROOT}/src/providers/portable/ice_transport.c"
  "${H2PEER_ROOT}/src/providers/portable/peer.c"
  "${H2PEER_ROOT}/src/providers/portable/peer_connection.c"
  "${H2PEER_ROOT}/src/providers/portable/ports.c"
  "${H2PEER_ROOT}/src/providers/portable/rtcp.c"
  "${H2PEER_ROOT}/src/providers/portable/rtp.c"
  "${H2PEER_ROOT}/src/providers/portable/sctp.c"
  "${H2PEER_ROOT}/src/providers/portable/sdp.c"
  "${H2PEER_ROOT}/src/providers/portable/socket.c"
  "${H2PEER_ROOT}/src/providers/portable/stun.c"
  "${H2PEER_ROOT}/src/providers/portable/utils.c")

add_library(h2peer STATIC
  "${H2PEER_ROOT}/src/h2_peer_webrtc.c"
  "${H2PEER_ROOT}/src/ice/h2_peer_ice.c"
  "${H2PEER_ROOT}/src/providers/h2_peer_portable_backend.c"
  "${H2PEER_ROOT}/src/turn/h2_peer_turn.c"
  ${H2PEER_PORTABLE_SOURCES})

target_include_directories(h2peer
  PUBLIC
    "${H2PEER_ROOT}/include"
    "${H2PEER_REPO_ROOT}/libs/pal/include"
  PRIVATE
    "${H2PEER_ROOT}/src"
    "${H2PEER_ROOT}/src/providers/portable")
target_compile_features(h2peer PUBLIC c_std_11)
target_compile_definitions(h2peer PRIVATE
  CONFIG_SDP_BUFFER_SIZE=12000)
target_compile_options(h2peer PRIVATE
  -Wall
  -Wextra
  -Wno-ignored-qualifiers
  -Wno-incompatible-pointer-types
  -Wno-sign-compare
  -Wno-unused-but-set-variable
  -Wno-unused-parameter
  -Wno-unused-variable)
target_link_libraries(h2peer PUBLIC h2libsrtp)
