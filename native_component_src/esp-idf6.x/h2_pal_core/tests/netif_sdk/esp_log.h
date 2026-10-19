#ifndef TEST_NETIF_ESP_LOG_H
#define TEST_NETIF_ESP_LOG_H
void test_netif_log(const char *tag, const char *format, ...);
#define ESP_LOGW test_netif_log
#endif
