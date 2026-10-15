#include "asm/sfc_norflash_api.h"
#include "asm/wdt.h"
#include "os/os_api.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2_jieli_warm_request.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t boot_info_get_sfc_base_addr(void);
static int armed;
static int renaming;
void h2_jieli_pref_rename_observer(int entering) {
  renaming = entering;
}
static unsigned char old_value[2048], new_value[2048];

void h2_jieli_pref_program_observer(uint32_t address, const void *buffer,
                                    uint32_t size) {
  if (!armed || !renaming || size != 256u) return;
  armed = 0;
  unsigned char before[256], after[256], expected[256];
  if (norflash_origin_read(before, address, 256u) != 256) return;
  memcpy(expected, before, 256u);
  unsigned first = 256u, last = 0u;
  for (unsigned i = 0; i < 256u; ++i) {
    if (before[i] != (before[i] & ((const unsigned char *)buffer)[i])) {
      if (first == 256u) first = i;
      last = i;
    }
  }
  printf("H2_JIELI_PREF_RENAME_PROGRAM address=%x first=%u last=%u\r\n",
         (unsigned)address, first, last);
  /* Change at least one byte but leave another changed byte unwritten. */
  if (first >= last) { armed = 1; return; }
  unsigned prefix = first + (last - first + 1u) / 2u;
  for (unsigned i = 0; i < prefix; ++i)
    expected[i] &= ((const unsigned char *)buffer)[i];
  (void)norflash_protect_suspend();
  int written = norflash_write(NULL, (void *)buffer, prefix, address);
  (void)norflash_protect_resume();
  int read = norflash_origin_read(after, address, 256u);
  int ok = written == (int)prefix && read == 256 && !memcmp(after, expected, 256u);
  wdt_close();
  for (;;) {
    printf("H2_JIELI_PREF_CUT_%s phase=rename address=%x size=256 prefix=%u\r\n",
           ok ? "READY" : "ERROR", (unsigned)address, prefix);
    os_time_dly(100u);
  }
}

/* Called only on entering command service. Persistent marker prevents rearming
 * after power loss; no Loader control keys are changed by this diagnostic. */
void h2_jieli_loader_pref_probe(const h2_pal_pref_api_t *pref,
                               const h2_pal_mem_api_t *mem) {
  if (boot_info_get_sfc_base_addr() != H2_JIELI_BANK_1_SFC_BASE) return;
  h2_pal_pref_namespace_t *ns = NULL;
  int rc = h2_pal_pref_open(pref, "powercut-rename-v2",
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) { printf("H2_JIELI_PREF_TEST open=%d\r\n", rc); return; }
  memset(old_value, 0x35, sizeof(old_value));
  memset(new_value, 0xca, sizeof(new_value));
  uint32_t marker = 0, sentinel = 0;
  rc = ns->get_u32(ns, "started", &marker);
  if (rc == H2_PAL_ERR_NOT_FOUND) {
    rc = ns->set_blob(ns, "value", old_value, sizeof(old_value));
    if (!rc) rc = ns->set_u32(ns, "sentinel", 0x51425374u);
    if (!rc) rc = ns->set_u32(ns, "started", 1u);
    if (!rc) {
      void *baseline = NULL;
      size_t size = 0;
      rc = ns->get_blob(ns, mem, "value", &baseline, &size);
      if (!rc && (size != sizeof(old_value) ||
                  memcmp(baseline, old_value, sizeof(old_value)))) rc = -1;
      h2_pal_mem_free(mem, baseline);
      if (!rc) rc = ns->get_u32(ns, "sentinel", &sentinel);
      if (!rc && sentinel != 0x51425374u) rc = -1;
    }
    if (!rc) {
      armed = 1;
      rc = ns->set_blob(ns, "value", new_value, sizeof(new_value));
      armed = 0;
      printf("H2_JIELI_PREF_TEST injection_missed=%d\r\n", rc);
    }
  } else if (!rc && marker == 1u) {
    void *value = NULL; size_t size = 0;
    rc = ns->get_blob(ns, mem, "value", &value, &size);
    int complete = !rc && size == sizeof(old_value) &&
        (!memcmp(value, old_value, size) || !memcmp(value, new_value, size));
    h2_pal_mem_free(mem, value);
    int sentinel_rc = ns->get_u32(ns, "sentinel", &sentinel);
    int retry = complete && !sentinel_rc && sentinel == 0x51425374u
        ? ns->set_blob(ns, "value", new_value, sizeof(new_value)) : -1;
    value = NULL; size = 0;
    int verify = retry ? retry : ns->get_blob(ns, mem, "value", &value, &size);
    int verified = !verify && size == sizeof(new_value) &&
        !memcmp(value, new_value, size);
    h2_pal_mem_free(mem, value);
    printf("H2_JIELI_PREF_RECOVERY complete=%d sentinel=%d retry=%d verified=%d\r\n",
           complete, !sentinel_rc && sentinel == 0x51425374u, retry, verified);
  }
  printf("H2_JIELI_PREF_TEST return=%d marker=%u\r\n", rc, (unsigned)marker);
  ns->close(ns);
}
