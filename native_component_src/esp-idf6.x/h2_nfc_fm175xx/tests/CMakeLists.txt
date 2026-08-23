cmake_minimum_required(VERSION 3.16)
project(h2_esp_nfc_fm175xx_tests C)

enable_testing()

add_executable(
  test_h2_esp_nfc_fm175xx_spi_address
  test_h2_esp_nfc_fm175xx_spi_address.c
  ../src/h2_esp_nfc_fm175xx_spi_address.c)

target_include_directories(
  test_h2_esp_nfc_fm175xx_spi_address
  PRIVATE
  ../src)

target_compile_features(test_h2_esp_nfc_fm175xx_spi_address PRIVATE c_std_11)
target_compile_options(
  test_h2_esp_nfc_fm175xx_spi_address
  PRIVATE
  -Wall
  -Wextra
  -Werror)

add_test(
  NAME h2_esp_nfc_fm175xx_spi_address
  COMMAND test_h2_esp_nfc_fm175xx_spi_address)
