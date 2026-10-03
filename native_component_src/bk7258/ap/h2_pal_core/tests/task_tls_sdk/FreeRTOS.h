#ifndef TEST_BK_FREERTOS_H
#define TEST_BK_FREERTOS_H
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 3
typedef void *TaskHandle_t;
void test_legacy_port_cleanup(void *task);
#define portCLEAN_UP_TCB(task) test_legacy_port_cleanup(task)
#endif
