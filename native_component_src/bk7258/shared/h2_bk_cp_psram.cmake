# BK7258 CP has a small internal heap. Keep the Wi-Fi reserve intact by
# placing ordinary CLI/DHCP service storage in the board's enabled PSRAM.
# Patch only guarded build-tree copies of the pinned SDK, never its checkout.
option(H2_BK_CP_PSRAM_SERVICES "Place non-radio CP services in PSRAM" ON)
if(H2_BK_CP_PSRAM_SERVICES)
  if(NOT CONFIG_PSRAM_AS_SYS_MEMORY OR CONFIG_FREERTOS_SMP)
    message(FATAL_ERROR "BK CP PSRAM service policy requires non-SMP PSRAM heap")
  endif()

  function(h2_bk_cp_patch_service component relative_path old_text new_text expected_count suffix)
    set(sdk_source "$ENV{ARMINO_PATH}/components/${relative_path}")
    file(READ "${sdk_source}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    set(remaining "${content}")
    string(LENGTH "${old_text}" old_length)
    set(count 0)
    while(TRUE)
      string(FIND "${remaining}" "${old_text}" offset)
      if(offset EQUAL -1)
        break()
      endif()
      math(EXPR count "${count} + 1")
      math(EXPR next "${offset} + ${old_length}")
      string(SUBSTRING "${remaining}" ${next} -1 remaining)
    endwhile()
    if(NOT count EQUAL expected_count)
      message(FATAL_ERROR "Pinned BK CP ${relative_path} changed: expected ${expected_count} anchors, found ${count}")
    endif()
    string(REPLACE "${old_text}" "${new_text}" corrected "${content}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_${suffix}.c")
    file(WRITE "${generated}" "${corrected}")

    armino_component_get_property(library ${component} COMPONENT_LIB)
    get_target_property(sources ${library} SOURCES)
    get_filename_component(source_name "${relative_path}" NAME)
    string(REPLACE "." "\\." source_pattern "${source_name}")
    set(found 0)
    foreach(source IN LISTS sources)
      if(source MATCHES "(^|/)${source_pattern}$")
        math(EXPR found "${found} + 1")
      endif()
    endforeach()
    if(NOT found EQUAL 1)
      message(FATAL_ERROR "Pinned BK CP ${component} source ${source_name} missing or duplicated")
    endif()
    list(FILTER sources EXCLUDE REGEX "(^|/)${source_pattern}$")
    list(APPEND sources "${generated}")
    set_property(TARGET ${library} PROPERTY SOURCES ${sources})
    get_filename_component(source_dir "${sdk_source}" DIRECTORY)
    # A generated source loses the original file's quote-include directory.
    # Put it before lwIP's public headers: dhcpd/dns.h and lwip/dns.h differ.
    target_include_directories(${library} BEFORE PRIVATE "${source_dir}")
  endfunction()

  # os_free handles both SRAM and PSRAM addresses in the pinned heap_4.c.
  h2_bk_cp_patch_service(
    lwip_intf_v2_1 lwip_intf_v2_1/dhcpd/dhcp-server.c
    "#define os_mem_alloc os_malloc" "#define os_mem_alloc psram_malloc" 1 dhcp_buffer)
  h2_bk_cp_patch_service(
    lwip_intf_v2_1 lwip_intf_v2_1/dhcpd/dhcp-server-main.c
    "rtos_create_thread(&dhcpd_thread," "rtos_create_psram_thread(&dhcpd_thread," 1 dhcp_task)

  # The CP CLI is a control-plane service, not a radio task. Its three SDK
  # build branches share the same handle and all use the existing PSRAM API.
  h2_bk_cp_patch_service(
    bk_cli bk_cli/cli_main.c
    "rtos_create_thread(&cli_thread_handle," "rtos_create_psram_thread(&cli_thread_handle," 3 cli_task)
endif()
