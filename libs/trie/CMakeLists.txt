if(COMMAND idf_component_register)
  idf_component_register(
      SRCS "src/h2_trie.c"
      INCLUDE_DIRS "include"
      REQUIRES h2_pal_core)
elseif(COMMAND armino_component_register)
  armino_component_register(
      SRCS "src/h2_trie.c"
      INCLUDE_DIRS "include"
      REQUIRES h2_pal_core)
else()
  message(FATAL_ERROR "h2_trie requires ESP-IDF or Armino component support")
endif()
