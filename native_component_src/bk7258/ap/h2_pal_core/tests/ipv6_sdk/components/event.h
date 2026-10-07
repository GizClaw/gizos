#ifndef H2_TEST_EVENT_H
#define H2_TEST_EVENT_H
#include <stddef.h>
#define BK_OK 0
#define BK_FAIL -1
#define BK_ERR_PARAM -2
#define BK_ERR_BUSY -3
#define BK_ERR_EVENT_CB_EXIST -4
typedef int bk_err_t;
typedef int event_module_t;
#define EVENT_MOD_WIFI 1
#define EVENT_WIFI_STA_CONNECTED 1
#define EVENT_WIFI_STA_DISCONNECTED 2
typedef bk_err_t (*test_event_callback_t)(void *, event_module_t, int, void *);
int bk_event_register_cb(event_module_t, int, test_event_callback_t, void *);
#endif
