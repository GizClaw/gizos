#include <libusb.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JIELI_RUNTIME_VID 0x3654
#define JIELI_RUNTIME_PID 0x5155

static volatile sig_atomic_t keep_running = 1;

static void stop_monitor(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static void timestamp(char *buffer, size_t size) {
  struct timespec now;
  struct tm local;
  clock_gettime(CLOCK_REALTIME, &now);
  localtime_r(&now.tv_sec, &local);
  snprintf(buffer, size, "%02d:%02d:%02d.%03ld", local.tm_hour,
           local.tm_min, local.tm_sec, now.tv_nsec / 1000000L);
}

static int find_cdc_interfaces(libusb_device *device, int *control_interface,
                               int *data_interface, uint8_t *endpoint) {
  struct libusb_config_descriptor *config = NULL;
  int result = libusb_get_active_config_descriptor(device, &config);
  if (result != LIBUSB_SUCCESS) {
    result = libusb_get_config_descriptor(device, 0, &config);
  }
  if (result != LIBUSB_SUCCESS) {
    return result;
  }

  *control_interface = -1;
  *data_interface = -1;
  result = LIBUSB_ERROR_NOT_FOUND;
  for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
    const struct libusb_interface *interface = &config->interface[i];
    for (int a = 0; a < interface->num_altsetting; ++a) {
      const struct libusb_interface_descriptor *alt = &interface->altsetting[a];
      if (alt->bInterfaceClass == LIBUSB_CLASS_COMM &&
          alt->bInterfaceSubClass == 2) {
        *control_interface = alt->bInterfaceNumber;
      }
      for (uint8_t e = 0; e < alt->bNumEndpoints; ++e) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                LIBUSB_ENDPOINT_IN &&
            (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                LIBUSB_TRANSFER_TYPE_BULK) {
          *data_interface = alt->bInterfaceNumber;
          *endpoint = ep->bEndpointAddress;
        }
      }
    }
  }

  if (*control_interface >= 0 && *data_interface >= 0 && *endpoint != 0) {
    result = LIBUSB_SUCCESS;
  }

  libusb_free_config_descriptor(config);
  return result;
}

int main(void) {
  libusb_context *context = NULL;
  libusb_device_handle *handle = NULL;
  int claimed_interface = -1;
  int last_error = LIBUSB_SUCCESS;
  char stamp[32];

  signal(SIGINT, stop_monitor);
  signal(SIGTERM, stop_monitor);

  int result = libusb_init(&context);
  if (result != LIBUSB_SUCCESS) {
    fprintf(stderr, "libusb_init: %s\n", libusb_error_name(result));
    return 1;
  }
  libusb_set_option(context, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
  setvbuf(stdout, NULL, _IONBF, 0);
  timestamp(stamp, sizeof(stamp));
  fprintf(stderr, "[%s] waiting for JieLi %04x:%04x on macOS\n", stamp,
          JIELI_RUNTIME_VID, JIELI_RUNTIME_PID);

  while (keep_running) {
    if (handle == NULL) {
      handle = libusb_open_device_with_vid_pid(context, JIELI_RUNTIME_VID,
                                                JIELI_RUNTIME_PID);
      if (handle == NULL) {
        usleep(50000);
        continue;
      }

      uint8_t endpoint = 0;
      int control_interface = -1;
      int interface_number = -1;
      result = find_cdc_interfaces(libusb_get_device(handle),
                                   &control_interface, &interface_number,
                                   &endpoint);
      if (result != LIBUSB_SUCCESS) {
        timestamp(stamp, sizeof(stamp));
        fprintf(stderr, "[%s] no bulk-IN endpoint: %s\n", stamp,
                libusb_error_name(result));
        libusb_close(handle);
        handle = NULL;
        usleep(50000);
        continue;
      }

      result = libusb_claim_interface(handle, interface_number);
      if (result != LIBUSB_SUCCESS) {
        timestamp(stamp, sizeof(stamp));
        fprintf(stderr, "[%s] claim interface %d: %s\n", stamp,
                interface_number, libusb_error_name(result));
        libusb_close(handle);
        handle = NULL;
        usleep(50000);
        continue;
      }
      claimed_interface = interface_number;

      /* JieLi does not enable the CDC endpoints until the ACM control
       * interface receives SET_CONTROL_LINE_STATE.  The data interface is
       * claimed for bulk I/O, but wIndex must name the paired control
       * interface. */
      result = libusb_control_transfer(
          handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
                      LIBUSB_RECIPIENT_INTERFACE,
          0x22, 0x0003, (uint16_t)control_interface, NULL, 0, 100);
      if (result < 0) {
        timestamp(stamp, sizeof(stamp));
        fprintf(stderr, "[%s] set control line state on interface %d: %s\n",
                stamp, control_interface, libusb_error_name(result));
        libusb_release_interface(handle, claimed_interface);
        claimed_interface = -1;
        libusb_close(handle);
        handle = NULL;
        usleep(50000);
        continue;
      }

      timestamp(stamp, sizeof(stamp));
      fprintf(stderr,
              "[%s] connected control=%d data=%d bulk_in=0x%02x\n", stamp,
              control_interface, interface_number, endpoint);
      last_error = LIBUSB_SUCCESS;

      while (keep_running) {
        unsigned char data[4096];
        int transferred = 0;
        result = libusb_bulk_transfer(handle, endpoint, data, sizeof(data),
                                      &transferred, 250);
        if (result == LIBUSB_SUCCESS && transferred > 0) {
          (void)fwrite(data, 1, (size_t)transferred, stdout);
          continue;
        }
        if (result == LIBUSB_ERROR_TIMEOUT) {
          continue;
        }
        last_error = result;
        break;
      }

      libusb_release_interface(handle, claimed_interface);
      claimed_interface = -1;
      libusb_close(handle);
      handle = NULL;
      timestamp(stamp, sizeof(stamp));
      fprintf(stderr, "\n[%s] disconnected: %s; waiting for re-enumeration\n",
              stamp, libusb_error_name(last_error));
    }
  }

  if (handle != NULL) {
    if (claimed_interface >= 0) {
      libusb_release_interface(handle, claimed_interface);
    }
    libusb_close(handle);
  }
  libusb_exit(context);
  return 0;
}
