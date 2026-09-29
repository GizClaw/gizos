set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_BK_PLATFORM_CORE_PRIV_REQUIRES
  bk_common
  bk_rtos
  controller_if)
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_core cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/pal/include"
  PRIV_REQUIRES
    ${H2_BK_PLATFORM_CORE_PRIV_REQUIRES})

# The public AP SDK disconnect lacks a radio-disassociate RPC; legacy
# BK_CMD_DISCONNECT stops the entire service. Add a paired private command
# to the pinned controller in a guarded build-tree copy, preserving the SDK.
set(H2_BK_CP_SDK_ROOT "$ENV{ARMINO_PATH}")
armino_component_get_property(H2_BK_CP_CONTROLLER_LIB controller_if COMPONENT_LIB)
set(H2_BK_CP_CONTROLLER_SOURCE "${H2_BK_CP_SDK_ROOT}/components/controller_if/cif_wifi_api.c")
file(READ "${H2_BK_CP_CONTROLLER_SOURCE}" H2_BK_CP_CONTROLLER_CONTENT)
string(REPLACE "\r\n" "\n" H2_BK_CP_CONTROLLER_CONTENT "${H2_BK_CP_CONTROLLER_CONTENT}")
set(H2_BK_CP_DISASSOC_ANCHOR "        case STA_PM_ENABLE:")
string(FIND "${H2_BK_CP_CONTROLLER_CONTENT}" "${H2_BK_CP_DISASSOC_ANCHOR}" H2_BK_CP_DISASSOC_OFFSET)
if(H2_BK_CP_DISASSOC_OFFSET EQUAL -1)
  message(FATAL_ERROR "Pinned BK CP API dispatch changed; revalidate disassociate RPC")
endif()
set(H2_BK_CP_DISASSOC_CASE "        case H2_BK_WIFI_RPC_STA_DISASSOCIATE:
        {
            ret = arg_info->argc == 0 ? bk_wifi_sta_disconnect() : BK_ERR_PARAM;
            break;
        }
${H2_BK_CP_DISASSOC_ANCHOR}")
string(REPLACE "${H2_BK_CP_DISASSOC_ANCHOR}" "${H2_BK_CP_DISASSOC_CASE}"
  H2_BK_CP_CONTROLLER_CORRECTED "${H2_BK_CP_CONTROLLER_CONTENT}")
set(H2_BK_CP_CONTROLLER_CORRECTED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_wifi_api.c")
file(WRITE "${H2_BK_CP_CONTROLLER_CORRECTED_SOURCE}"
  "#include \"h2_bk_wifi_rpc.h\"\n${H2_BK_CP_CONTROLLER_CORRECTED}")
get_target_property(H2_BK_CP_CONTROLLER_SOURCES ${H2_BK_CP_CONTROLLER_LIB} SOURCES)
list(FILTER H2_BK_CP_CONTROLLER_SOURCES EXCLUDE REGEX "(^|/)cif_wifi_api\\.c$")
list(APPEND H2_BK_CP_CONTROLLER_SOURCES "${H2_BK_CP_CONTROLLER_CORRECTED_SOURCE}")
set_property(TARGET ${H2_BK_CP_CONTROLLER_LIB} PROPERTY SOURCES ${H2_BK_CP_CONTROLLER_SOURCES})
target_include_directories(${H2_BK_CP_CONTROLLER_LIB} PRIVATE
  "${REPO_ROOT}/native_component_src/bk7258/shared")
