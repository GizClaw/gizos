cmake_minimum_required(VERSION 3.16)
project(h2_esp_adc_stabilizer_tests C)

enable_testing()

add_executable(
  test_h2_esp_adc_stabilizer
  test_h2_esp_adc_stabilizer.c
  ../src/h2_esp_adc_radio_button.c
  ../src/h2_esp_adc_stabilizer.c)

target_include_directories(
  test_h2_esp_adc_stabilizer
  PRIVATE
  ../include
  ../src)

target_compile_features(test_h2_esp_adc_stabilizer PRIVATE c_std_11)
target_compile_options(
  test_h2_esp_adc_stabilizer
  PRIVATE
  -Wall
  -Wextra
  -Werror)

add_test(NAME h2_esp_adc_stabilizer COMMAND test_h2_esp_adc_stabilizer)
