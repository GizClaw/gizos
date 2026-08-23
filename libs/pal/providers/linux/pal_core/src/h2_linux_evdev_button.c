#define _POSIX_C_SOURCE 200809L

#include "h2_linux_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define H2_LINUX_EVDEV_SCAN_MAX 64u
#define H2_LINUX_EVDEV_PATH_MAX_LEN 64u
#define H2_LINUX_BITS_PER_WORD (sizeof(unsigned long) * 8u)
#define H2_LINUX_KEY_WORD_COUNT                                                \
  (((size_t)KEY_MAX + H2_LINUX_BITS_PER_WORD) / H2_LINUX_BITS_PER_WORD)

typedef struct linux_evdev_button_entry {
  h2_pal_periph_id_t periph_id;
  char device_name[H2_LINUX_EVDEV_DEVICE_NAME_MAX_LEN];
  char device_path[H2_LINUX_EVDEV_PATH_MAX_LEN];
  uint16_t key_code;
} linux_evdev_button_entry_t;

static linux_evdev_button_entry_t
    s_linux_evdev_buttons[H2_LINUX_EVDEV_BUTTON_MAX];
static size_t s_linux_evdev_button_count;

#if defined(H2_LINUX_TESTING)
static h2_linux_evdev_test_read_fn s_linux_evdev_test_reader;
static void *s_linux_evdev_test_reader_user;
#endif

static linux_evdev_button_entry_t *
linux_evdev_find_button(h2_pal_periph_id_t periph_id) {
  for (size_t index = 0u; index < s_linux_evdev_button_count; ++index) {
    if (s_linux_evdev_buttons[index].periph_id == periph_id) {
      return &s_linux_evdev_buttons[index];
    }
  }
  return NULL;
}

static h2_pal_result_t linux_evdev_read_path(const char *path,
                                             const char *expected_name,
                                             uint16_t key_code,
                                             h2_pal_button_state_t *out_state) {
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
  if (strcmp(actual_name, expected_name) != 0) {
    (void)close(fd);
    return H2_PAL_ERR_NOT_FOUND;
  }

  unsigned long key_bits[H2_LINUX_KEY_WORD_COUNT];
  memset(key_bits, 0, sizeof(key_bits));
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
    (void)close(fd);
    return H2_PAL_ERR_IO;
  }
  const size_t word = (size_t)key_code / H2_LINUX_BITS_PER_WORD;
  const size_t bit = (size_t)key_code % H2_LINUX_BITS_PER_WORD;
  if ((key_bits[word] & (1ul << bit)) == 0ul) {
    (void)close(fd);
    return H2_PAL_ERR_NOT_FOUND;
  }
  memset(key_bits, 0, sizeof(key_bits));
  if (ioctl(fd, EVIOCGKEY(sizeof(key_bits)), key_bits) < 0) {
    (void)close(fd);
    return H2_PAL_ERR_IO;
  }
  if (close(fd) != 0) {
    return H2_PAL_ERR_IO;
  }
  *out_state = (key_bits[word] & (1ul << bit)) != 0ul
                   ? H2_PAL_BUTTON_STATE_PRESSED
                   : H2_PAL_BUTTON_STATE_RELEASED;
  return H2_PAL_OK;
}

static h2_pal_result_t
linux_evdev_discover_and_read(linux_evdev_button_entry_t *entry,
                              h2_pal_button_state_t *out_state) {
#if defined(H2_LINUX_TESTING)
  if (s_linux_evdev_test_reader != NULL) {
    return s_linux_evdev_test_reader(s_linux_evdev_test_reader_user,
                                     entry->device_name, entry->key_code,
                                     out_state);
  }
#endif
  if (entry->device_path[0] != '\0') {
    h2_pal_result_t result = linux_evdev_read_path(
        entry->device_path, entry->device_name, entry->key_code, out_state);
    if (result == H2_PAL_OK) {
      return result;
    }
    entry->device_path[0] = '\0';
  }

  h2_pal_result_t first_error = H2_PAL_ERR_NOT_FOUND;
  for (unsigned int index = 0u; index < H2_LINUX_EVDEV_SCAN_MAX; ++index) {
    char path[H2_LINUX_EVDEV_PATH_MAX_LEN];
    const int written =
        snprintf(path, sizeof(path), "/dev/input/event%u", index);
    if (written < 0 || (size_t)written >= sizeof(path)) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = linux_evdev_read_path(path, entry->device_name,
                                                   entry->key_code, out_state);
    if (result == H2_PAL_OK) {
      memcpy(entry->device_path, path, (size_t)written + 1u);
      return H2_PAL_OK;
    }
    if (result != H2_PAL_ERR_NOT_FOUND && first_error == H2_PAL_ERR_NOT_FOUND) {
      first_error = result;
    }
  }
  return first_error;
}

static h2_pal_result_t
linux_evdev_read_single_button(void *user, h2_pal_periph_id_t periph_id,
                               h2_pal_single_button_reading_t *out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  linux_evdev_button_entry_t *entry = linux_evdev_find_button(periph_id);
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_button_state_t state = H2_PAL_BUTTON_STATE_RELEASED;
  h2_pal_result_t result = linux_evdev_discover_and_read(entry, &state);
  if (result != H2_PAL_OK) {
    return result;
  }
  *out_reading = (h2_pal_single_button_reading_t){
      .id = periph_id,
      .state = state,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t linux_evdev_read_radio_button_group(
    void *user, h2_pal_periph_id_t periph_id,
    h2_pal_radio_button_group_reading_t *out_reading) {
  (void)user;
  (void)periph_id;
  (void)out_reading;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_button_vtable_t s_linux_evdev_button_vtable = {
    .read_single_button = linux_evdev_read_single_button,
    .read_radio_button_group = linux_evdev_read_radio_button_group,
};

static const h2_pal_button_api_t s_linux_evdev_button_api = {
    .user = NULL,
    .vtable = &s_linux_evdev_button_vtable,
};

h2_pal_result_t
h2_linux_configure_evdev_buttons(const h2_linux_evdev_button_config_t *entries,
                                 size_t count) {
  if (count > H2_LINUX_EVDEV_BUTTON_MAX || (count != 0u && entries == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  linux_evdev_button_entry_t configured[H2_LINUX_EVDEV_BUTTON_MAX];
  memset(configured, 0, sizeof(configured));
  for (size_t index = 0u; index < count; ++index) {
    const h2_linux_evdev_button_config_t *source = &entries[index];
    if (source->periph_id == 0u || source->device_name == NULL ||
        source->device_name[0] == '\0' || source->key_code > KEY_MAX) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t name_len = strlen(source->device_name);
    if (name_len >= sizeof(configured[index].device_name)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t prior = 0u; prior < index; ++prior) {
      if (configured[prior].periph_id == source->periph_id) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
    configured[index].periph_id = source->periph_id;
    configured[index].key_code = source->key_code;
    memcpy(configured[index].device_name, source->device_name, name_len + 1u);
  }
  memcpy(s_linux_evdev_buttons, configured, sizeof(configured));
  s_linux_evdev_button_count = count;
  return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_linux_evdev_button_api(void) {
  return &s_linux_evdev_button_api;
}

#if defined(H2_LINUX_TESTING)
void h2_linux_evdev_test_set_reader(h2_linux_evdev_test_read_fn read_fn,
                                    void *user) {
  s_linux_evdev_test_reader = read_fn;
  s_linux_evdev_test_reader_user = user;
}
#endif
