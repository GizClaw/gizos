#define _POSIX_C_SOURCE 200809L

#include "h2_linux_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define H2_LINUX_GPIO_PATH_MAX_LEN 512u

typedef struct linux_gpio_button_entry {
  h2_pal_periph_id_t periph_id;
  char chip_label[H2_LINUX_GPIO_CHIP_LABEL_MAX_LEN];
  uint32_t line_offset;
  int active_low;
  int line_fd;
} linux_gpio_button_entry_t;

static linux_gpio_button_entry_t
    s_linux_gpio_buttons[H2_LINUX_GPIO_BUTTON_MAX];
static size_t s_linux_gpio_button_count;

#if defined(H2_LINUX_TESTING)
static h2_linux_gpio_test_read_fn s_linux_gpio_test_reader;
static void *s_linux_gpio_test_reader_user;
#endif

static h2_pal_result_t linux_gpio_result_from_errno(int error_number) {
  if (error_number == ENOENT || error_number == ENODEV) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (error_number == EBUSY) {
    return H2_PAL_ERR_BUSY;
  }
  if (error_number == EACCES || error_number == EPERM) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  return H2_PAL_ERR_IO;
}

static linux_gpio_button_entry_t *
linux_gpio_find_button(h2_pal_periph_id_t periph_id) {
  for (size_t index = 0u; index < s_linux_gpio_button_count; ++index) {
    if (s_linux_gpio_buttons[index].periph_id == periph_id) {
      return &s_linux_gpio_buttons[index];
    }
  }
  return NULL;
}

static int linux_gpio_is_chip_node_name(const char *name) {
  static const char prefix[] = "gpiochip";
  if (name == NULL || strncmp(name, prefix, sizeof(prefix) - 1u) != 0) {
    return 0;
  }
  const char *suffix = name + sizeof(prefix) - 1u;
  if (*suffix == '\0') {
    return 0;
  }
  while (*suffix != '\0') {
    if (*suffix < '0' || *suffix > '9') {
      return 0;
    }
    ++suffix;
  }
  return 1;
}

static h2_pal_result_t linux_gpio_discovery_result(
    int has_match, h2_pal_result_t candidate_error) {
  if (candidate_error != H2_PAL_ERR_NOT_FOUND) {
    return candidate_error;
  }
  return has_match != 0 ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t
linux_gpio_request_line(linux_gpio_button_entry_t *entry) {
  h2_pal_result_t first_error = H2_PAL_ERR_NOT_FOUND;
  int matched_chip_fd = -1;
  DIR *directory = opendir("/dev");
  if (directory == NULL) {
    return linux_gpio_result_from_errno(errno);
  }

  errno = 0;
  struct dirent *node = NULL;
  while ((node = readdir(directory)) != NULL) {
    if (linux_gpio_is_chip_node_name(node->d_name) == 0) {
      continue;
    }
    char path[H2_LINUX_GPIO_PATH_MAX_LEN];
    const int written = snprintf(path, sizeof(path), "/dev/%s", node->d_name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
      first_error = H2_PAL_ERR_INVALID_STATE;
      continue;
    }
    const int chip_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
      h2_pal_result_t result = linux_gpio_result_from_errno(errno);
      if (result != H2_PAL_ERR_NOT_FOUND &&
          first_error == H2_PAL_ERR_NOT_FOUND) {
        first_error = result;
      }
      continue;
    }

    struct gpiochip_info chip_info;
    memset(&chip_info, 0, sizeof(chip_info));
    if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
      h2_pal_result_t result = linux_gpio_result_from_errno(errno);
      if (close(chip_fd) != 0) {
        return H2_PAL_ERR_IO;
      }
      if (first_error == H2_PAL_ERR_NOT_FOUND) {
        first_error = result;
      }
      continue;
    }
    if (strncmp(chip_info.label, entry->chip_label,
                sizeof(chip_info.label)) != 0 ||
        entry->line_offset >= chip_info.lines) {
      if (close(chip_fd) != 0) {
        return H2_PAL_ERR_IO;
      }
      continue;
    }
    if (matched_chip_fd >= 0) {
      const int current_close_result = close(chip_fd);
      const int matched_close_result = close(matched_chip_fd);
      const int directory_close_result = closedir(directory);
      if (current_close_result != 0 || matched_close_result != 0 ||
          directory_close_result != 0) {
        return H2_PAL_ERR_IO;
      }
      return H2_PAL_ERR_INVALID_STATE;
    }
    matched_chip_fd = chip_fd;
  }

  const int read_directory_error = errno;
  if (closedir(directory) != 0) {
    if (matched_chip_fd >= 0) {
      (void)close(matched_chip_fd);
    }
    return H2_PAL_ERR_IO;
  }
  if (read_directory_error != 0) {
    if (matched_chip_fd >= 0) {
      (void)close(matched_chip_fd);
    }
    return linux_gpio_result_from_errno(read_directory_error);
  }
  if (matched_chip_fd < 0) {
    return first_error;
  }
  h2_pal_result_t discovery_result =
      linux_gpio_discovery_result(1, first_error);
  if (discovery_result != H2_PAL_OK) {
    if (close(matched_chip_fd) != 0) {
      return H2_PAL_ERR_IO;
    }
    return discovery_result;
  }

  struct gpiohandle_request request;
  memset(&request, 0, sizeof(request));
  request.lineoffsets[0] = entry->line_offset;
  request.flags = GPIOHANDLE_REQUEST_INPUT;
  if (entry->active_low != 0) {
    request.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;
  }
  request.lines = 1u;
  const char consumer[] = "h2-linux-button";
  memcpy(request.consumer_label, consumer, sizeof(consumer));
  if (ioctl(matched_chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
    h2_pal_result_t result = linux_gpio_result_from_errno(errno);
    if (close(matched_chip_fd) != 0) {
      return H2_PAL_ERR_IO;
    }
    return result;
  }
  if (close(matched_chip_fd) != 0) {
    if (close(request.fd) != 0) {
      return H2_PAL_ERR_IO;
    }
    return H2_PAL_ERR_IO;
  }
  entry->line_fd = request.fd;
  return H2_PAL_OK;
}

static h2_pal_result_t
linux_gpio_read_entry(linux_gpio_button_entry_t *entry,
                      h2_pal_button_state_t *out_state) {
#if defined(H2_LINUX_TESTING)
  if (s_linux_gpio_test_reader != NULL) {
    return s_linux_gpio_test_reader(
        s_linux_gpio_test_reader_user, entry->chip_label, entry->line_offset,
        entry->active_low, out_state);
  }
#endif
  if (entry->line_fd < 0) {
    h2_pal_result_t result = linux_gpio_request_line(entry);
    if (result != H2_PAL_OK) {
      return result;
    }
  }

  struct gpiohandle_data data;
  memset(&data, 0, sizeof(data));
  if (ioctl(entry->line_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0) {
    const int read_error = errno;
    if (close(entry->line_fd) != 0) {
      entry->line_fd = -1;
      return H2_PAL_ERR_IO;
    }
    entry->line_fd = -1;
    return linux_gpio_result_from_errno(read_error);
  }
  *out_state = data.values[0] != 0u ? H2_PAL_BUTTON_STATE_PRESSED
                                    : H2_PAL_BUTTON_STATE_RELEASED;
  return H2_PAL_OK;
}

static h2_pal_result_t
linux_gpio_read_single_button(void *user, h2_pal_periph_id_t periph_id,
                              h2_pal_single_button_reading_t *out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  linux_gpio_button_entry_t *entry = linux_gpio_find_button(periph_id);
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_button_state_t state = H2_PAL_BUTTON_STATE_RELEASED;
  h2_pal_result_t result = linux_gpio_read_entry(entry, &state);
  if (result != H2_PAL_OK) {
    return result;
  }
  *out_reading = (h2_pal_single_button_reading_t){
      .id = periph_id,
      .state = state,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t linux_gpio_read_radio_button_group(
    void *user, h2_pal_periph_id_t periph_id,
    h2_pal_radio_button_group_reading_t *out_reading) {
  (void)user;
  (void)periph_id;
  (void)out_reading;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_button_vtable_t s_linux_gpio_button_vtable = {
    .read_single_button = linux_gpio_read_single_button,
    .read_radio_button_group = linux_gpio_read_radio_button_group,
};

static const h2_pal_button_api_t s_linux_gpio_button_api = {
    .user = NULL,
    .vtable = &s_linux_gpio_button_vtable,
};

h2_pal_result_t
h2_linux_configure_gpio_buttons(const h2_linux_gpio_button_config_t *entries,
                                size_t count) {
  if (count > H2_LINUX_GPIO_BUTTON_MAX || (count != 0u && entries == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  linux_gpio_button_entry_t configured[H2_LINUX_GPIO_BUTTON_MAX];
  memset(configured, 0, sizeof(configured));
  for (size_t index = 0u; index < H2_LINUX_GPIO_BUTTON_MAX; ++index) {
    configured[index].line_fd = -1;
  }
  for (size_t index = 0u; index < count; ++index) {
    const h2_linux_gpio_button_config_t *source = &entries[index];
    if (source->periph_id == 0u || source->chip_label == NULL ||
        source->chip_label[0] == '\0' ||
        (source->active_low != 0 && source->active_low != 1)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t label_len = strlen(source->chip_label);
    if (label_len >= sizeof(configured[index].chip_label)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t prior = 0u; prior < index; ++prior) {
      if (configured[prior].periph_id == source->periph_id) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
    configured[index].periph_id = source->periph_id;
    configured[index].line_offset = source->line_offset;
    configured[index].active_low = source->active_low;
    memcpy(configured[index].chip_label, source->chip_label, label_len + 1u);
  }

  int close_failed = 0;
  for (size_t index = 0u; index < s_linux_gpio_button_count; ++index) {
    if (s_linux_gpio_buttons[index].line_fd >= 0 &&
        close(s_linux_gpio_buttons[index].line_fd) != 0) {
      close_failed = 1;
    }
    s_linux_gpio_buttons[index].line_fd = -1;
  }
  if (close_failed != 0) {
    return H2_PAL_ERR_IO;
  }
  memcpy(s_linux_gpio_buttons, configured, sizeof(configured));
  s_linux_gpio_button_count = count;
  return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_linux_gpio_button_api(void) {
  return &s_linux_gpio_button_api;
}

#if defined(H2_LINUX_TESTING)
void h2_linux_gpio_test_set_reader(h2_linux_gpio_test_read_fn read_fn,
                                   void *user) {
  s_linux_gpio_test_reader = read_fn;
  s_linux_gpio_test_reader_user = user;
}

h2_pal_result_t h2_linux_gpio_test_discovery_result(
    int has_match, h2_pal_result_t candidate_error) {
  return linux_gpio_discovery_result(has_match, candidate_error);
}

int h2_linux_gpio_test_is_chip_node_name(const char *name) {
  return linux_gpio_is_chip_node_name(name);
}
#endif
