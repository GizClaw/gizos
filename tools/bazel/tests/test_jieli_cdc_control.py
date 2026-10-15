"""Exercise the actual vendor CDC handlers after applying the board patch.

Run with JIELI_AC791N_SDK_PATH set to the pinned SDK checkout. Endpoint I/O
is mocked; this verifies configuration lifetime, not hardware reliability.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
CDC = Path("apps/common/usb/device/cdc.c")
PATCH = ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_patches/early_app_boot.patch"


def function(source, name, prefix="static "):
    start = source.index(prefix + name)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r"""
typedef unsigned char u8;
typedef unsigned int u32;
typedef int usb_dev;
#define NULL ((void *)0)
#define BIT(n) (1u << (n))
#define USB_TYPE_MASK 0x60
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_MASK 0x1f
#define USB_CDC_REQ_SET_LINE_CODING 0x20
#define USB_CDC_REQ_GET_LINE_CODING 0x21
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE 0x22
#define USB_EP0_STAGE_SETUP 0
#define USB_EP0_SET_STALL 1
#define CDC_DATA_EP_IN 3
#define CDC_INTR_EP_ENABLE 0
#define log_debug(...) ((void)0)
#define log_error(...) ((void)0)
struct usb_device_t { int unused; };
struct usb_ctrlrequest { u8 bRequestType, bRequest; unsigned short wValue, wIndex, wLength; };
struct usb_cdc_line_coding { char bytes[7]; };
struct cdc_handle { u8 bmTransceiver; char subtype_data[16]; } handle;
static struct cdc_handle *cdc_hdl[] = { &handle };
static int configurations;
static int usb_device2id(struct usb_device_t *d) { return 0; }
static void cdc_endpoint_init(struct usb_device_t *d, u32 itf) { ++configurations; }
static u32 cdc_setup_rx(struct usb_device_t *d, struct usb_ctrlrequest *r) { return 0; }
static void usb_set_setup_recv(struct usb_device_t *d, u32 (*f)(struct usb_device_t *, struct usb_ctrlrequest *)) {}
static void usb_set_setup_phase(struct usb_device_t *d, int phase) {}
static void usb_set_data_payload(struct usb_device_t *d, struct usb_ctrlrequest *r, void *data, u32 len) {}
static void dump_line_coding(struct usb_cdc_line_coding *coding) {}
static void usb_disable_ep(int id, int ep) {}
"""

MAIN = r"""
int main(void) {
    struct usb_device_t device = {0};
    struct usb_ctrlrequest request = {
        USB_TYPE_CLASS, USB_CDC_REQ_SET_CONTROL_LINE_STATE, 0, 0, 0
    };
    cdc_setup(&device, &request);
    if (configurations != 1) return 10;
    request.wValue = 3;
    cdc_setup(&device, &request);
    request.wValue = 0;
    cdc_setup(&device, &request);
    if (configurations != 1) return 11;
    cdc_reset(&device, 0);
    if (handle.bmTransceiver & BIT(4)) return 12;
    int before = configurations;
    cdc_setup(&device, &request);
    if (configurations != before + 1) return 13;
    cdc_setup(&device, &request);
    if (configurations != before + 1) return 14;
    return 0;
}
"""


class CdcControlTests(unittest.TestCase):
    def test_rx_trace_preserves_read_and_lock_failure(self):
        sdk = os.environ.get("JIELI_AC791N_SDK_PATH")
        if not sdk:
            self.skipTest("JIELI_AC791N_SDK_PATH is required")
        with tempfile.TemporaryDirectory(prefix="h2-cdc-rx-trace-") as directory:
            temp = Path(directory)
            target = temp / CDC
            target.parent.mkdir(parents=True)
            shutil.copyfile(Path(sdk) / CDC, target)
            subprocess.run(["git", "apply", "--include=" + str(CDC), str(PATCH)],
                           cwd=temp, check=True, capture_output=True)
            harness = temp / "rx.c"
            harness.write_text(r'''
#include <string.h>
typedef unsigned int u32;
typedef unsigned char u8;
typedef int usb_dev;
#define OS_NO_ERR 0
#define CDC_DATA_EP_OUT 2
#define MAXP_SIZE_CDC_BULKOUT 64
static u8 input[64] = {1, 2, 3};
static struct { u8 *cdc_buffer; int mutex_data; } handle = {input, 0};
static __typeof__(&handle) cdc_hdl[] = {&handle};
static int busy, reads, posts, phases[4], phase_count;
static int os_mutex_pend(int *mutex, int ticks) { return busy; }
static void os_mutex_post(int *mutex) { ++posts; }
static u32 usb_g_bulk_read(int id, int ep, u8 *out, u32 size, int block) {
    ++reads; return 3;
}
void h2_jieli_usb_cdc_rx_trace(u32 phase, u32 bytes) __attribute__((weak));
void h2_jieli_usb_cdc_rx_trace(u32 phase, u32 bytes) {
    phases[phase_count++] = phase;
    if (phase == 4 && bytes != 3) phases[3] = -1;
}
''' + function(target.read_text(), "u32 cdc_read_data(", prefix="") + r'''
int main(void) {
    u8 out[8] = {0};
    busy = 1;
    if (cdc_read_data(0, out, sizeof(out)) != 0 || reads || posts) return 1;
    if (phase_count != 2 || phases[0] != 1 || phases[1] != 2) return 2;
    busy = phase_count = 0;
    if (cdc_read_data(0, out, sizeof(out)) != 3 || reads != 1 || posts != 1) return 3;
    if (memcmp(out, input, 3) != 0) return 4;
    if (phase_count != 3 || phases[0] != 1 || phases[1] != 3 || phases[2] != 4 || phases[3] == -1) return 5;
    return 0;
}
''')
            executable = temp / "rx"
            subprocess.run(["cc", "-std=c11", str(harness), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_configuration_lifetime(self):
        sdk = os.environ.get("JIELI_AC791N_SDK_PATH")
        if not sdk:
            self.skipTest("JIELI_AC791N_SDK_PATH is required")
        with tempfile.TemporaryDirectory(prefix="h2-cdc-control-") as directory:
            temp = Path(directory)
            target = temp / CDC
            target.parent.mkdir(parents=True)
            shutil.copyfile(Path(sdk) / CDC, target)
            original = target.read_text()
            subprocess.run(["git", "apply", "--include=" + str(CDC), str(PATCH)],
                           cwd=temp, check=True, capture_output=True)
            patched = target.read_text()
            for root2 in (0, 1):
                for label, source in (("original", original), ("patched", patched)):
                    with self.subTest(root2=root2, source=label):
                        harness = temp / "handler.c"
                        harness.write_text(STUBS + function(source, "u32 cdc_setup(")
                                           + function(source, "void cdc_reset(") + MAIN)
                        executable = temp / "handler"
                        subprocess.run(["cc", "-std=c11", f"-DUSB_ROOT2={root2}",
                                        str(harness), "-o", str(executable)], check=True)
                        result = subprocess.run([str(executable)])
                        self.assertEqual(result.returncode, 11 if label == "original" else 0)


if __name__ == "__main__":
    unittest.main()
