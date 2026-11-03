#ifndef H2_ES8311_E2E_TEST_ESP_CLK_H
#define H2_ES8311_E2E_TEST_ESP_CLK_H
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
static inline int esp_clk_cpu_freq(void) { return 160000000; }
#endif
