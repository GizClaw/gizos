#ifndef TEST_MOSAICO_MODULE_CAMERA_H
#define TEST_MOSAICO_MODULE_CAMERA_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
enum {ESP_OK=0, ESP_ERR_TIMEOUT=1, ESP_ERR_NO_MEM=2, ESP_ERR_INVALID_ARG=3,
      ESP_ERR_INVALID_STATE=4, ESP_ERR_NOT_FOUND=5, ESP_ERR_NOT_SUPPORTED=6};
#define V4L2_PIX_FMT_UYVY 1
#define MOSAICO_MODULE_MGR_SLOT_LEFT 0
#define MOSAICO_CAMERA_DEFAULT_CONFIG() {0}
typedef void *mosaico_camera_handle_t;
typedef struct {int slot; uint32_t width, height; bool apply_module_tuning;} mosaico_camera_config_t;
typedef struct {const void *data; size_t size; uint32_t index, width, height, bytes_per_line, pixel_format;} mosaico_camera_frame_t;
int mosaico_camera_new(const mosaico_camera_config_t *, mosaico_camera_handle_t *);
int mosaico_camera_open(mosaico_camera_handle_t);
int mosaico_camera_start_stream(mosaico_camera_handle_t);
int mosaico_camera_get_frame(mosaico_camera_handle_t, mosaico_camera_frame_t *);
int mosaico_camera_return_frame(mosaico_camera_handle_t, const mosaico_camera_frame_t *);
int mosaico_camera_del(mosaico_camera_handle_t);
#endif
