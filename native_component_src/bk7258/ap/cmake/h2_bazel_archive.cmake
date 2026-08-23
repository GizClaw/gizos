function(h2_bk_import_bazel_archive target variable)
  set(archive "$ENV{${variable}}")
  if(NOT archive OR NOT EXISTS "${archive}")
    message(FATAL_ERROR
      "${target} requires the Bazel-built ${variable} archive")
  endif()
  add_library(${target}_bazel STATIC IMPORTED GLOBAL)
  set_target_properties(${target}_bazel PROPERTIES IMPORTED_LOCATION "${archive}")
  set(imported_targets ${target}_bazel)

  set(index 0)
  while(TRUE)
    set(dependency_variable "${variable}_DEPENDENCY_${index}")
    set(dependency_archive "$ENV{${dependency_variable}}")
    if(NOT dependency_archive)
      break()
    endif()
    if(NOT EXISTS "${dependency_archive}")
      message(FATAL_ERROR
        "${target} requires the Bazel-built ${dependency_variable} archive")
    endif()
    add_library(${target}_bazel_dependency_${index} STATIC IMPORTED GLOBAL)
    set_target_properties(
      ${target}_bazel_dependency_${index}
      PROPERTIES IMPORTED_LOCATION "${dependency_archive}")
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
