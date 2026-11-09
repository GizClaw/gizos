#ifndef H2_JIELI_CONSOLE_TEST_SDK_H
#define H2_JIELI_CONSOLE_TEST_SDK_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned usb_dev;
typedef struct {
  int live, held;
} OS_MUTEX;
typedef struct {
  int live, count;
} OS_SEM;
struct usb_device_t {
  usb_dev id;
};
enum { OS_NO_ERR = 0, USB_MAX_HW_NUM = 2 };
uint32_t timer_get_ms(void);
void os_time_dly(unsigned ticks);
int os_mutex_create(OS_MUTEX* mutex);
int os_mutex_del(OS_MUTEX* mutex, int force);
int os_mutex_accept(OS_MUTEX* mutex);
int os_mutex_pend(OS_MUTEX* mutex, int ticks);
int os_mutex_post(OS_MUTEX* mutex);
int os_sem_create(OS_SEM* sem, int count);
int os_sem_del(OS_SEM* sem, int force);
int os_sem_pend(OS_SEM* sem, unsigned ticks);
int os_sem_post(OS_SEM* sem);
int os_task_create(void (*entry)(void*), void* user, int priority, int stack,
                   int queue, const char* name);
usb_dev usb_device2id(struct usb_device_t* device);
uint32_t cdc_read_data(usb_dev id, uint8_t* data, uint32_t len);
uint32_t cdc_write_data(usb_dev id, uint8_t* data, uint32_t len);
void cdc_set_wakeup_handler(usb_dev id, void (*handler)(struct usb_device_t*));

#endif
