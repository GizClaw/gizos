#include "esp_private/c_api_wrapper.hpp"

extern "C" void *lierda_test_dce_create(void) { return new esp_modem::DCE; }
extern "C" void lierda_test_dce_destroy(void *driver) {
    delete static_cast<esp_modem::DCE *>(driver);
}

esp_modem::command_result esp_modem::DCE::command(
    const std::string &wire, got_line_cb callback, uint32_t timeout) {
    const int result = lierda_test_exchange(wire.c_str(), &callback,
        [](void *user, uint8_t *data, size_t size) {
            const auto rc = (*static_cast<got_line_cb *>(user))(data, size);
            return rc == command_result::OK ? ESP_OK
                : rc == command_result::FAIL ? ESP_FAIL : ESP_ERR_TIMEOUT;
        }, timeout);
    return result == ESP_OK ? command_result::OK
        : result == ESP_FAIL ? command_result::FAIL : command_result::TIMEOUT;
}
