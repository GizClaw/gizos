# Explicit Board stack configuration wins over the pinned SDK's KEYVALUE/FTP
# fallback. Generate an owned build-tree header; never change the sealed SDK.
armino_component_get_property(H2_BK_LWIP_CONFIG_LIB lwip_intf_v2_1 COMPONENT_LIB)
set(H2_BK_LWIP_OPTIONS_SOURCE
  "$ENV{ARMINO_PATH}/components/lwip_intf_v2_1/lwip-2.1.2/port/lwipopts.h")
file(READ "${H2_BK_LWIP_OPTIONS_SOURCE}" H2_BK_LWIP_OPTIONS)
string(REPLACE "\r\n" "\n" H2_BK_LWIP_OPTIONS "${H2_BK_LWIP_OPTIONS}")
set(H2_BK_LWIP_STACK_PATTERN
  "#if[^\n]*CONFIG_KEYVALUE[^\n]*\n#define TCPIP_THREAD_STACKSIZE[ \t]+1024\n#elif[^\n]*CONFIG_LWIP_TCPIP_THREAD_STACKSIZE[^\n]*\n#define TCPIP_THREAD_STACKSIZE[^\n]*\n#endif")
string(REGEX MATCHALL "${H2_BK_LWIP_STACK_PATTERN}"
  H2_BK_LWIP_STACK_BLOCKS "${H2_BK_LWIP_OPTIONS}")
list(LENGTH H2_BK_LWIP_STACK_BLOCKS H2_BK_LWIP_STACK_COUNT)
if(NOT H2_BK_LWIP_STACK_COUNT EQUAL 1)
  message(FATAL_ERROR "Pinned BK lwIP stack policy changed; revalidate the Board config mapping")
endif()
list(GET H2_BK_LWIP_STACK_BLOCKS 0 H2_BK_LWIP_STACK_BLOCK)
string(REGEX MATCH "^[^\n]*" H2_BK_LWIP_STACK_GUARD "${H2_BK_LWIP_STACK_BLOCK}")
# Retain each SDK unit's own fallback condition (CP: KEYVALUE; AP: also FTP).
string(REPLACE "#ifdef CONFIG_KEYVALUE" "#if defined(CONFIG_KEYVALUE)"
  H2_BK_LWIP_STACK_GUARD "${H2_BK_LWIP_STACK_GUARD}")
string(REGEX REPLACE "^#if" "#elif"
  H2_BK_LWIP_STACK_FALLBACK "${H2_BK_LWIP_STACK_GUARD}")
string(REPLACE "${H2_BK_LWIP_STACK_BLOCK}"
  "#if defined(CONFIG_LWIP_TCPIP_THREAD_STACKSIZE)
#define TCPIP_THREAD_STACKSIZE CONFIG_LWIP_TCPIP_THREAD_STACKSIZE
${H2_BK_LWIP_STACK_FALLBACK}
#define TCPIP_THREAD_STACKSIZE 1024
#endif"
  H2_BK_LWIP_OPTIONS "${H2_BK_LWIP_OPTIONS}")
set(H2_BK_LWIP_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_lwip_config")
file(MAKE_DIRECTORY "${H2_BK_LWIP_CONFIG_DIR}")
file(WRITE "${H2_BK_LWIP_CONFIG_DIR}/lwipopts.h" "${H2_BK_LWIP_OPTIONS}")
target_include_directories(${H2_BK_LWIP_CONFIG_LIB} BEFORE PUBLIC
  "${H2_BK_LWIP_CONFIG_DIR}")
