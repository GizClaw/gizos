# The generator verifies the pinned real SDK source SHA and writes only into
# this build directory. Its exact output is also consumed by the real NOR test.
set(H2_BK_FLASHDB_KV_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/h2_fdb_kvdb.c")
execute_process(
  COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/h2_fdb_gc_overlay.py"
    --input "${BK_FLASHDB_DIR}/src/fdb_kvdb.c"
    --output "${H2_BK_FLASHDB_KV_SOURCE}"
  RESULT_VARIABLE H2_FDB_OVERLAY_RESULT
  ERROR_VARIABLE H2_FDB_OVERLAY_ERROR)
if(NOT H2_FDB_OVERLAY_RESULT EQUAL 0)
  message(FATAL_ERROR "FlashDB overlay failed: ${H2_FDB_OVERLAY_ERROR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BK_FLASHDB_DIR}/src/fdb_kvdb.c"
  "${CMAKE_CURRENT_LIST_DIR}/h2_fdb_gc_overlay.py"
  "${CMAKE_CURRENT_LIST_DIR}/h2_fdb_move_kv.inc"
  "${CMAKE_CURRENT_LIST_DIR}/h2_fdb_do_gc.inc")
