function(h2_idf_import_bazel_archive target variable)
  if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "" OR
     NOT EXISTS "${${variable}}")
    message(FATAL_ERROR
      "${target} requires the Bazel-built ${variable} archive")
  endif()
  add_prebuilt_library(${target}_bazel "${${variable}}")
  set(imported_targets ${target}_bazel)

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
    list(APPEND imported_targets ${target}_bazel_dependency_${index})
    math(EXPR index "${index} + 1")
  endwhile()

  # Bazel's CcInfo closure can contain circular references across archives.
  # Keep the libraries as separate physical archives, but expose them to the
  # native linker as one rescan group owned by this firmware component.
  target_link_libraries(
    ${COMPONENT_LIB}
    INTERFACE
      "-Wl,--start-group"
      ${imported_targets}
      "-Wl,--end-group")
endfunction()
