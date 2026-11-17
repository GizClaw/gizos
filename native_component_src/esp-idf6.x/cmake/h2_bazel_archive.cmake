# CMake must preserve archive groups as a link-graph item. Raw flags can be
# separated from their imported targets during transitive link ordering.
if(CMAKE_VERSION VERSION_LESS "3.24")
  message(FATAL_ERROR "Bazel ESP archive rescan requires CMake 3.24 or newer")
endif()
# ESP-IDF uses GNU ld with a Generic system, where the builtin RESCAN feature
# is not provided. Cache scope also exposes the feature to the final ELF target.
set(CMAKE_LINK_GROUP_USING_h2_idf_rescan_SUPPORTED TRUE CACHE INTERNAL
  "ESP-IDF GNU archive rescan" FORCE)
set(CMAKE_LINK_GROUP_USING_h2_idf_rescan
  "-Wl,--start-group" "-Wl,--end-group" CACHE INTERNAL
  "ESP-IDF GNU archive rescan delimiters" FORCE)
set(CMAKE_LINK_LIBRARY_USING_h2_idf_whole_SUPPORTED TRUE CACHE INTERNAL
  "ESP-IDF GNU alwayslink archives" FORCE)
set(CMAKE_LINK_LIBRARY_USING_h2_idf_whole
  "-Wl,--whole-archive" "<LINK_ITEM>" "-Wl,--no-whole-archive" CACHE INTERNAL
  "ESP-IDF GNU alwayslink delimiters" FORCE)

function(h2_idf_import_bazel_archive target variable)
  if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "" OR
     NOT EXISTS "${${variable}}")
    message(FATAL_ERROR
      "${target} requires the Bazel-built ${variable} archive")
  endif()
  add_prebuilt_library(${target}_bazel "${${variable}}")
  if("${${variable}}" MATCHES "[.]lo$")
    set(imported_targets "$<LINK_LIBRARY:h2_idf_whole,${target}_bazel>")
  else()
    set(imported_targets ${target}_bazel)
  endif()

  set(index 0)
  while(TRUE)
    set(dependency_variable "${variable}_DEPENDENCY_${index}")
    if(NOT DEFINED ${dependency_variable})
      break()
    endif()
    if("${${dependency_variable}}" STREQUAL "" OR
       NOT EXISTS "${${dependency_variable}}")
      message(FATAL_ERROR
        "${target} requires the Bazel-built ${dependency_variable} archive")
    endif()
    add_prebuilt_library(
      ${target}_bazel_dependency_${index}
      "${${dependency_variable}}")
    if("${${dependency_variable}}" MATCHES "[.]lo$")
      list(APPEND imported_targets
        "$<LINK_LIBRARY:h2_idf_whole,${target}_bazel_dependency_${index}>")
    else()
      list(APPEND imported_targets ${target}_bazel_dependency_${index})
    endif()
    math(EXPR index "${index} + 1")
  endwhile()

  # Bazel's CcInfo closure can contain circular references across archives.
  # Keep the libraries as separate physical archives, but expose them to the
  # native linker as one rescan group owned by this firmware component.
  string(JOIN "," group_targets ${imported_targets})
  target_link_libraries(
    ${COMPONENT_LIB} INTERFACE
    "$<LINK_GROUP:h2_idf_rescan,${group_targets}>")
endfunction()
