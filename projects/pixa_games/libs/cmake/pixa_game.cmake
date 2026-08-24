# Adds one portable PIXA game library to an existing firmware component without
# creating a platform-specific component wrapper for the game.
get_filename_component(
  H2_PIXA_GAMES_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../.."
  ABSOLUTE)

function(h2_pixa_game_attach target game)
  if(NOT DEFINED H2_PIXA_UPSTREAM_ROOT OR H2_PIXA_UPSTREAM_ROOT STREQUAL "")
    message(FATAL_ERROR
      "H2_PIXA_UPSTREAM_ROOT must point to the verified Bazel vendor archive")
  endif()
  if(NOT DEFINED H2_PIXELROOT32_UPSTREAM_ROOT OR
     H2_PIXELROOT32_UPSTREAM_ROOT STREQUAL "")
    message(FATAL_ERROR
      "H2_PIXELROOT32_UPSTREAM_ROOT must point to the verified Bazel vendor archive")
  endif()
  set(h2_pixa_game_root
      "${H2_PIXA_GAMES_REPO_ROOT}/projects/pixa_games/libs/${game}")
  set(h2_pixelroot32_root "${H2_PIXELROOT32_UPSTREAM_ROOT}")

  if(game STREQUAL "dinobounce")
    set(h2_pixa_game_sources
        "${h2_pixa_game_root}/src/dinobounce.cpp")
  elseif(game STREQUAL "dinodive")
    set(h2_pixa_game_sources
        "${h2_pixa_game_root}/src/dinodive.cpp"
        "${h2_pixa_game_root}/src/dinodive_audio.cpp")
  elseif(game STREQUAL "dinorun")
    set(h2_pixa_game_sources
        "${h2_pixa_game_root}/src/dinorun.cpp"
        "${h2_pixa_game_root}/src/dinorun_audio.cpp")
  elseif(game STREQUAL "dinotetris")
    set(h2_pixa_game_sources
        "${h2_pixa_game_root}/src/dinotetris.cpp")
  elseif(game STREQUAL "tuxemon")
    set(h2_pixa_game_sources
        "${h2_pixa_game_root}/src/tuxemon.cpp"
        "${h2_pixa_game_root}/src/generated/tuxemon_assets.cpp"
        "${h2_pixelroot32_root}/src/graphics/Camera2D.cpp")
  else()
    message(FATAL_ERROR "unsupported embedded PIXA game: ${game}")
  endif()

  set(h2_pixa_game_target "h2_pixa_game_${game}")
  add_library("${h2_pixa_game_target}" OBJECT ${h2_pixa_game_sources})
  target_include_directories("${h2_pixa_game_target}" PRIVATE
    "${h2_pixa_game_root}/include"
    "${h2_pixa_game_root}/src"
    "${H2_PIXA_GAMES_REPO_ROOT}/libs/game_runtime/include"
    "${H2_PIXA_GAMES_REPO_ROOT}/libs/game_runtime/compat"
    "${H2_PIXA_GAMES_REPO_ROOT}/libs/pixa/include"
    "${H2_PIXA_GAMES_REPO_ROOT}/libs/pal/include"
    "${H2_PIXA_UPSTREAM_ROOT}/pkgs/c/include"
    "${h2_pixelroot32_root}/include")
  target_compile_options("${h2_pixa_game_target}" PRIVATE
    -std=c++17
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror)
  target_compile_definitions("${h2_pixa_game_target}" PRIVATE
    PIXELROOT32_ENABLE_AUDIO=0
    PIXELROOT32_ENABLE_PHYSICS=1
    PIXELROOT32_ENABLE_UI_SYSTEM=0
    PIXELROOT32_ENABLE_PARTICLES=0
    PIXELROOT32_ENABLE_CAMERA_EFFECTS=0
    PIXELROOT32_ENABLE_SCENE_TRANSITIONS=0
    PIXELROOT32_ENABLE_STATIC_TILEMAP_FB_CACHE=0
    PIXELROOT32_NO_TFT_ESPI=1)
  if(game STREQUAL "tuxemon")
    target_compile_definitions("${h2_pixa_game_target}" PRIVATE
      PIXELROOT32_ENABLE_4BPP_SPRITES=1)
  endif()

  target_sources("${target}" PRIVATE
    $<TARGET_OBJECTS:${h2_pixa_game_target}>)
  target_include_directories("${target}" PRIVATE
    "${h2_pixa_game_root}/include")
endfunction()
