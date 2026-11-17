#ifndef H2_NFC_TEST_ESP_LOG_H
#define H2_NFC_TEST_ESP_LOG_H

void h2_nfc_test_log(const char* tag, const char* format, ...);

#define ESP_LOGW(...) h2_nfc_test_log(__VA_ARGS__)

#endif
