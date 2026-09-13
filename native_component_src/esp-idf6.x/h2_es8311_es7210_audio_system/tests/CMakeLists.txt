cmake_minimum_required(VERSION 3.16)
project(h2_esp_es8311_es7210_gain_tests C)

enable_testing()

add_executable(
  test_h2_esp_es8311_es7210_gain
  test_h2_esp_es8311_es7210_gain.c
  ../src/h2_esp_es8311_es7210_gain.c)

target_include_directories(
  test_h2_esp_es8311_es7210_gain
  PRIVATE
  ../src)

target_compile_features(test_h2_esp_es8311_es7210_gain PRIVATE c_std_11)
target_compile_options(
  test_h2_esp_es8311_es7210_gain
  PRIVATE
  -Wall
  -Wextra
  -Werror)

add_test(
  NAME h2_esp_es8311_es7210_gain
  COMMAND test_h2_esp_es8311_es7210_gain)

add_executable(
  test_h2_esp_es8311_es7210_sr
  test_h2_esp_es8311_es7210_sr.c
  fake_esp_aec.c
  ../src/h2_esp_es8311_es7210_sr.c)

target_include_directories(
  test_h2_esp_es8311_es7210_sr
  PRIVATE
  include)

target_compile_features(test_h2_esp_es8311_es7210_sr PRIVATE c_std_11)
target_compile_options(
  test_h2_esp_es8311_es7210_sr
  PRIVATE
  -Wall
  -Wextra
  -Werror)

add_test(
  NAME h2_esp_es8311_es7210_sr
  COMMAND test_h2_esp_es8311_es7210_sr)

add_executable(test_power_down
  test_power_down.c
  ../src/h2_esp_es8311_es7210_audio_system.c
  ../src/h2_esp_es8311_es7210_gain.c
  ../../../../libs/drivers/audio/es8311/src/h2_es8311_volume.c)
target_include_directories(test_power_down PRIVATE
  sdk ../include
  ../../../../libs/audio_mixer/include
  ../../../../libs/pal/include
  ../../../../libs/drivers/audio/es8311/include)
target_compile_features(test_power_down PRIVATE c_std_11)
target_compile_options(test_power_down PRIVATE -Wall -Wextra -Werror)
add_test(NAME power_down COMMAND test_power_down)
