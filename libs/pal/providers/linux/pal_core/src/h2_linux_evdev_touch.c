#define _POSIX_C_SOURCE 200809L

#include "h2_linux_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct h2_linux_evdev_touch_state {
  h2_linux_evdev_touch_config_t config;
  char device_name[H2_LINUX_EVDEV_DEVICE_NAME_MAX_LEN];
  char device_path[PATH_MAX];
  int fd;
  int configured;
  int opened;
  struct input_absinfo x_axis;
  struct input_absinfo y_axis;
  int raw_x;
  int raw_y;
  int x_valid;
  int y_valid;
  int contact_valid;
  int pressed;
  int reported_pressed;
  int reported_x;
  int reported_y;
  int reported;
} h2_linux_evdev_touch_state_t;

static h2_linux_evdev_touch_state_t s_touch = {.fd = -1};

static int touch_is_event_node_name(const char *name) {
  static const char prefix[] = "event";
  if (name == NULL || strncmp(name, prefix, sizeof(prefix) - 1u) != 0) {
    return 0;
  }
  const char *suffix = name + sizeof(prefix) - 1u;
  if (*suffix == '\0') {
    return 0;
  }
  for (; *suffix != '\0'; ++suffix) {
    if (*suffix < '0' || *suffix > '9') {
      return 0;
    }
  }
  return 1;
}

static void touch_reset_report_state(void) {
  s_touch.raw_x = 0;
  s_touch.raw_y = 0;
  s_touch.x_valid = 0;
  s_touch.y_valid = 0;
  s_touch.contact_valid = 0;
  s_touch.pressed = 0;
  s_touch.reported_pressed = 0;
  s_touch.reported_x = 0;
  s_touch.reported_y = 0;
  s_touch.reported = 0;
}

static h2_pal_result_t touch_close_fd(void) {
  h2_pal_result_t result = H2_PAL_OK;
  if (s_touch.fd >= 0 && close(s_touch.fd) != 0) {
    result = H2_PAL_ERR_IO;
  }
  s_touch.fd = -1;
  s_touch.opened = 0;
  touch_reset_report_state();
  return result;
}

static h2_pal_result_t touch_open_path(const char *path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return errno == ENOENT || errno == ENODEV ? H2_PAL_ERR_NOT_FOUND
                                              : H2_PAL_ERR_IO;
  }
  char actual_name[H2_LINUX_EVDEV_DEVICE_NAME_MAX_LEN];
  memset(actual_name, 0, sizeof(actual_name));
  if (ioctl(fd, EVIOCGNAME(sizeof(actual_name)), actual_name) < 0) {
    (void)close(fd);
    return H2_PAL_ERR_IO;
  }
  if (strcmp(actual_name, s_touch.device_name) != 0) {
    (void)close(fd);
    return H2_PAL_ERR_NOT_FOUND;
  }
  struct input_absinfo x_axis;
  struct input_absinfo y_axis;
  if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x_axis) < 0 ||
      ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y_axis) < 0 ||
      x_axis.maximum <= x_axis.minimum || y_axis.maximum <= y_axis.minimum) {
    (void)close(fd);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  s_touch.fd = fd;
  s_touch.x_axis = x_axis;
  s_touch.y_axis = y_axis;
  s_touch.opened = 1;
  touch_reset_report_state();
  return H2_PAL_OK;
}

static h2_pal_result_t touch_open(void *user) {
  (void)user;
  if (!s_touch.configured) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (s_touch.opened) {
    return H2_PAL_OK;
  }
  if (s_touch.device_path[0] != '\0') {
    h2_pal_result_t result = touch_open_path(s_touch.device_path);
    if (result == H2_PAL_OK) {
      return result;
    }
    s_touch.device_path[0] = '\0';
  }
  DIR *directory = opendir("/dev/input");
  if (directory == NULL) {
    return errno == ENOENT ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_IO;
  }
  h2_pal_result_t first_error = H2_PAL_ERR_NOT_FOUND;
  struct dirent *node = NULL;
  for (;;) {
    errno = 0;
    node = readdir(directory);
    if (node == NULL) {
      break;
    }
    if (!touch_is_event_node_name(node->d_name)) {
      continue;
    }
    char path[PATH_MAX];
    const int written =
        snprintf(path, sizeof(path), "/dev/input/%s", node->d_name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
      if (first_error == H2_PAL_ERR_NOT_FOUND) {
        first_error = H2_PAL_ERR_INVALID_STATE;
      }
      continue;
    }
    h2_pal_result_t result = touch_open_path(path);
    if (result == H2_PAL_OK) {
      memcpy(s_touch.device_path, path, (size_t)written + 1u);
      if (closedir(directory) != 0) {
        (void)touch_close_fd();
        return H2_PAL_ERR_IO;
      }
      return H2_PAL_OK;
    }
    if (result != H2_PAL_ERR_NOT_FOUND && first_error == H2_PAL_ERR_NOT_FOUND) {
      first_error = result;
    }
  }
  const int read_error = errno;
  if (closedir(directory) != 0 || read_error != 0) {
    return H2_PAL_ERR_IO;
  }
  return first_error;
}

static int32_t touch_scale_axis(int value, const struct input_absinfo *axis,
                                uint32_t logical_size) {
  int clamped = value;
  if (clamped < axis->minimum) {
    clamped = axis->minimum;
  } else if (clamped > axis->maximum) {
    clamped = axis->maximum;
  }
  const uint64_t offset =
      (uint64_t)((int64_t)clamped - (int64_t)axis->minimum);
  const uint64_t range =
      (uint64_t)((int64_t)axis->maximum - (int64_t)axis->minimum);
  const uint64_t numerator = offset * (uint64_t)(logical_size - 1u);
  return (int32_t)(numerator / range);
}

static void touch_map_coordinates(int *out_x, int *out_y) {
  int x = touch_scale_axis(s_touch.raw_x, &s_touch.x_axis,
                           s_touch.config.width);
  int y = touch_scale_axis(s_touch.raw_y, &s_touch.y_axis,
                           s_touch.config.height);
  if (s_touch.config.swap_xy) {
    const int swapped = x;
    x = y;
    y = swapped;
  }
  const uint32_t output_width = s_touch.config.swap_xy
                                    ? s_touch.config.height
                                    : s_touch.config.width;
  const uint32_t output_height = s_touch.config.swap_xy
                                     ? s_touch.config.width
                                     : s_touch.config.height;
  if (s_touch.config.invert_x) {
    x = (int)output_width - 1 - x;
  }
  if (s_touch.config.invert_y) {
    y = (int)output_height - 1 - y;
  }
  *out_x = x;
  *out_y = y;
}

static h2_pal_result_t touch_finish_report(h2_pal_touch_event_t *out_event) {
  const int initial_coordinates_valid = s_touch.x_valid && s_touch.y_valid;
  const int coordinate_update_valid =
      s_touch.reported && (s_touch.x_valid || s_touch.y_valid);
  if (!initial_coordinates_valid && !s_touch.reported) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  int x = s_touch.reported_x;
  int y = s_touch.reported_y;
  if (initial_coordinates_valid || coordinate_update_valid) {
    touch_map_coordinates(&x, &y);
  }

  h2_pal_touch_event_kind_t kind;
  if (!s_touch.reported) {
    if (!s_touch.contact_valid || !s_touch.pressed ||
        !initial_coordinates_valid) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    kind = H2_PAL_TOUCH_EVENT_DOWN;
  } else if (s_touch.pressed != s_touch.reported_pressed) {
    if (!s_touch.contact_valid ||
        (s_touch.pressed && !initial_coordinates_valid)) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    kind = s_touch.pressed ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP;
  } else if (s_touch.pressed &&
             (x != s_touch.reported_x || y != s_touch.reported_y)) {
    kind = H2_PAL_TOUCH_EVENT_MOVE;
  } else {
    return H2_PAL_ERR_WOULD_BLOCK;
  }

  s_touch.reported = 1;
  s_touch.reported_pressed = s_touch.pressed;
  s_touch.reported_x = x;
  s_touch.reported_y = y;
  *out_event = (h2_pal_touch_event_t){
      .kind = kind,
      .x = x,
      .y = y,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t touch_process_event(uint16_t type, uint16_t code,
                                           int32_t value,
                                           h2_pal_touch_event_t *out_event) {
  if (type == EV_KEY && code == BTN_TOUCH) {
    s_touch.pressed = value != 0;
    s_touch.contact_valid = 1;
  } else if (type == EV_ABS && code == ABS_MT_POSITION_X) {
    s_touch.raw_x = value;
    s_touch.x_valid = 1;
  } else if (type == EV_ABS && code == ABS_MT_POSITION_Y) {
    s_touch.raw_y = value;
    s_touch.y_valid = 1;
  } else if (type == EV_SYN && code == SYN_DROPPED) {
    touch_reset_report_state();
    return H2_PAL_ERR_IO;
  } else if (type == EV_SYN && code == SYN_REPORT) {
    const h2_pal_result_t result = touch_finish_report(out_event);
    s_touch.x_valid = 0;
    s_touch.y_valid = 0;
    s_touch.contact_valid = 0;
    return result;
  }
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t touch_get_info(void *user,
                                      h2_pal_touch_info_t *out_info) {
  (void)user;
  if (!s_touch.configured) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  *out_info = (h2_pal_touch_info_t){
      .width = s_touch.config.swap_xy ? s_touch.config.height
                                      : s_touch.config.width,
      .height = s_touch.config.swap_xy ? s_touch.config.width
                                       : s_touch.config.height,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(void *user,
                                        h2_pal_touch_event_t *out_event) {
  (void)user;
  if (!s_touch.opened || s_touch.fd < 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  for (;;) {
    struct input_event event;
    const ssize_t bytes_read = read(s_touch.fd, &event, sizeof(event));
    if (bytes_read < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return H2_PAL_ERR_WOULD_BLOCK;
      }
      if (errno == ENODEV) {
        (void)touch_close_fd();
        return H2_PAL_ERR_CLOSED;
      }
      return H2_PAL_ERR_IO;
    }
    if (bytes_read == 0) {
      (void)touch_close_fd();
      return H2_PAL_ERR_CLOSED;
    }
    if ((size_t)bytes_read != sizeof(event)) {
      return H2_PAL_ERR_IO;
    }
    h2_pal_result_t result = touch_process_event(
        event.type, event.code, event.value, out_event);
    if (result != H2_PAL_ERR_WOULD_BLOCK) {
      return result;
    }
  }
}

static h2_pal_result_t touch_close(void *user) {
  (void)user;
  return touch_close_fd();
}

static const h2_pal_touch_vtable_t s_touch_vtable = {
    .open = touch_open,
    .get_info = touch_get_info,
    .poll_event = touch_poll_event,
    .close = touch_close,
};

static const h2_pal_touch_api_t s_touch_api = {
    .user = NULL,
    .vtable = &s_touch_vtable,
};

h2_pal_result_t h2_linux_configure_evdev_touch(
    const h2_linux_evdev_touch_config_t *config) {
  if (config == NULL || config->device_name == NULL ||
      config->device_name[0] == '\0' || config->width == 0u ||
      config->height == 0u || config->width > (uint32_t)INT32_MAX ||
      config->height > (uint32_t)INT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t name_len = strlen(config->device_name);
  if (name_len >= sizeof(s_touch.device_name)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t result = touch_close_fd();
  if (result != H2_PAL_OK) {
    return result;
  }
  memset(&s_touch, 0, sizeof(s_touch));
  s_touch.fd = -1;
  s_touch.config = *config;
  memcpy(s_touch.device_name, config->device_name, name_len + 1u);
  s_touch.config.device_name = s_touch.device_name;
  s_touch.configured = 1;
  return H2_PAL_OK;
}

const h2_pal_touch_api_t *h2_linux_evdev_touch_api(void) {
  return &s_touch_api;
}

#if defined(H2_LINUX_TESTING)
int h2_linux_evdev_touch_test_is_event_node_name(const char *name) {
  return touch_is_event_node_name(name);
}

h2_pal_result_t h2_linux_evdev_touch_test_set_axes(
    int32_t x_minimum,
    int32_t x_maximum,
    int32_t y_minimum,
    int32_t y_maximum) {
  if (!s_touch.configured) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (x_maximum <= x_minimum || y_maximum <= y_minimum) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  s_touch.x_axis.minimum = x_minimum;
  s_touch.x_axis.maximum = x_maximum;
  s_touch.y_axis.minimum = y_minimum;
  s_touch.y_axis.maximum = y_maximum;
  return H2_PAL_OK;
}

h2_pal_result_t h2_linux_evdev_touch_test_feed(
    uint16_t type, uint16_t code, int32_t value,
    h2_pal_touch_event_t *out_event) {
  if (out_event == NULL || !s_touch.configured) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_touch.x_axis.maximum <= s_touch.x_axis.minimum ||
      s_touch.y_axis.maximum <= s_touch.y_axis.minimum) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return touch_process_event(type, code, value, out_event);
}
#endif
