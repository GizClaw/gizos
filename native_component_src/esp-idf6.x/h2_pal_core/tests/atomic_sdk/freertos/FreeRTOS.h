#ifndef H2_ATOMIC_TEST_FREERTOS_H
#define H2_ATOMIC_TEST_FREERTOS_H
extern int h2_test_critical_entries;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_SAFE(lock) ((void)(lock), ++h2_test_critical_entries)
#define portEXIT_CRITICAL_SAFE(lock) ((void)(lock))
#endif
