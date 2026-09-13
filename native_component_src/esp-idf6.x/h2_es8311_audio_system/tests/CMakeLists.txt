cmake_minimum_required(VERSION 3.16)
project(h2_esp_es8311_power_tests C)
enable_testing()
if(MSVC)
  add_compile_options(/W4 /WX /UNDEBUG)
else()
  add_compile_options(-Wall -Wextra -Werror -UNDEBUG)
endif()

add_executable(test_power_down
  test_power_down.c
  ../src/h2_esp_es8311_audio_system.c
  ../src/h2_esp_es8311_gain.c
  ../../../../libs/drivers/audio/es8311/src/h2_es8311_volume.c
  ../../../../libs/drivers/audio/es8311/src/h2_es8311_power.c)
target_include_directories(test_power_down PRIVATE
  ../../h2_es8311_es7210_audio_system/tests/sdk ../include
  ../../../../libs/audio_mixer/include
  ../../../../libs/pal/include
  ../../../../libs/drivers/audio/es8311/include)
target_compile_features(test_power_down PRIVATE c_std_11)
add_test(NAME power_down COMMAND test_power_down)
