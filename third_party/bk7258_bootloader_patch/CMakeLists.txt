cmake_minimum_required(VERSION 3.20)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(bk7258_mapped_ab C ASM)

foreach(required INPUT_IMAGE PARTITION_METADATA OUTPUT_IMAGE OUTPUT_RECEIPT PYTHON CMAKE_OBJCOPY)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} must be supplied by the native build")
  endif()
endforeach()

add_executable(mapped_ab mapped_ab.c mapped_ab.S)
target_compile_options(mapped_ab PRIVATE -std=c11 -Wall -Wextra -Werror -Os
  -mcpu=cortex-m33 -mthumb -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables)
target_link_options(mapped_ab PRIVATE -nostdlib -mcpu=cortex-m33 -mthumb
  "-T${CMAKE_CURRENT_SOURCE_DIR}/mapped_ab.ld" -Wl,--build-id=none)
set_property(TARGET mapped_ab APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mapped_ab.ld")

add_custom_command(OUTPUT "${OUTPUT_IMAGE}" "${OUTPUT_RECEIPT}"
  COMMAND "${CMAKE_OBJCOPY}" -O binary -j .text "$<TARGET_FILE:mapped_ab>" payload.bin
  COMMAND "${CMAKE_OBJCOPY}" -O binary -j .branch "$<TARGET_FILE:mapped_ab>" branch.bin
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/apply.py"
    --manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    --image "${INPUT_IMAGE}" --metadata "${PARTITION_METADATA}"
    --payload "${CMAKE_CURRENT_BINARY_DIR}/payload.bin"
    --branch "${CMAKE_CURRENT_BINARY_DIR}/branch.bin"
    --output "${OUTPUT_IMAGE}" --receipt "${OUTPUT_RECEIPT}"
  DEPENDS mapped_ab "${INPUT_IMAGE}" "${PARTITION_METADATA}/partitions.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/apply.py" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
  VERBATIM)
add_custom_target(apply_bootloader ALL DEPENDS "${OUTPUT_IMAGE}" "${OUTPUT_RECEIPT}")
