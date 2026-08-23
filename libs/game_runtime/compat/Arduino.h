#ifndef H2_GAME_RUNTIME_COMPAT_ARDUINO_H
#define H2_GAME_RUNTIME_COMPAT_ARDUINO_H

#include <stdint.h>

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef INPUT
#define INPUT 0
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 1
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif

#ifdef __cplusplus

#if defined(ESP_PLATFORM)
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif defined(BEKEN_PLATFORM)
extern "C" {
uint32_t rtos_get_time(void);
int rtos_delay_milliseconds(uint32_t ms);
void bk_delay_us(uint32_t us);
}
#else
#include <chrono>
#include <thread>
#endif

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

class H2PixelRootSerial {
public:
    void begin(unsigned long) {}
    void print(const char *) {}
    void println(const char *) {}
    void printf(const char *, ...) {}
};

inline H2PixelRootSerial Serial;

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline int digitalRead(int) { return HIGH; }
inline int analogRead(uint8_t) { return 0; }

inline uint32_t millis() {
#if defined(ESP_PLATFORM)
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
#elif defined(BEKEN_PLATFORM)
    return rtos_get_time();
#else
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
#endif
}

inline uint32_t micros() {
#if defined(ESP_PLATFORM)
    return static_cast<uint32_t>(esp_timer_get_time());
#elif defined(BEKEN_PLATFORM)
    return rtos_get_time() * 1000;
#else
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
#endif
}

inline void delay(uint32_t ms) {
#if defined(ESP_PLATFORM)
    vTaskDelay(pdMS_TO_TICKS(ms));
#elif defined(BEKEN_PLATFORM)
    (void)rtos_delay_milliseconds(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

inline void delayMicroseconds(uint32_t us) {
#if defined(ESP_PLATFORM)
    esp_rom_delay_us(us);
#elif defined(BEKEN_PLATFORM)
    bk_delay_us(us);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}

inline void yield() {
#if defined(ESP_PLATFORM)
    vTaskDelay(1);
#elif defined(BEKEN_PLATFORM)
    (void)rtos_delay_milliseconds(1);
#else
    std::this_thread::yield();
#endif
}

#endif

#endif
