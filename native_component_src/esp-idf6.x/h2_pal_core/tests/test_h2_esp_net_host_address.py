"""Execute the production ESP host-address entry with controlled SDK inventories."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]

SDK = r'''
#include "h2/pal/net/h2_pal_net.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LWIP_IPV6 1
#define CONFIG_LWIP_IPV6_NUM_ADDRESSES 3
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { uint32_t addr[4]; } esp_ip6_addr_t;
typedef struct esp_netif {
  const char *key, *name;
  int index, up;
  esp_netif_ip_info_t ip4;
  esp_ip6_addr_t ip6[3];
  unsigned preferred[3];
  unsigned count;
} esp_netif_t;
static esp_netif_t interfaces[2];
static esp_netif_t *default_netif;
static int in_tcpip, fail_tcpip;
static unsigned callbacks;
static int family_to_lwip(h2_pal_net_family_t family) {
  return family == H2_PAL_NET_FAMILY_IPV4 ? 4 :
         family == H2_PAL_NET_FAMILY_IPV6 ? 6 : -1;
}
static esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *user) {
  assert(!in_tcpip);
  if (fail_tcpip) return ESP_FAIL;
  ++callbacks;
  in_tcpip = 1;
  int rc = callback(user);
  in_tcpip = 0;
  return rc;
}
static esp_netif_t *esp_netif_get_default_netif(void) {
  assert(in_tcpip);
  return default_netif;
}
static esp_netif_t *esp_netif_next_unsafe(esp_netif_t *netif) {
  assert(in_tcpip);
  return netif == NULL ? &interfaces[0] :
         netif == &interfaces[0] ? &interfaces[1] : NULL;
}
static esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name) {
  strcpy(name, netif->name);
  return ESP_OK;
}
static const char *esp_netif_get_ifkey(esp_netif_t *netif) { return netif->key; }
static const char *esp_netif_get_desc(esp_netif_t *netif) { return netif->key; }
static bool esp_netif_is_netif_up(esp_netif_t *netif) { return netif->up != 0; }
static int esp_netif_get_netif_impl_index(esp_netif_t *netif) { return netif->index; }
static esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
  assert(in_tcpip);
  *out = netif->ip4;
  return ESP_OK;
}
int esp_netif_get_all_ip6(esp_netif_t *netif, esp_ip6_addr_t *out) {
  assert(in_tcpip && netif->count <= 3u);
  memcpy(out, netif->ip6, netif->count * sizeof(*out));
  return (int)netif->count;
}
int esp_netif_get_all_preferred_ip6(esp_netif_t *netif, esp_ip6_addr_t *out) {
  assert(in_tcpip && netif->count <= 3u);
  int count = 0;
  for (unsigned i = 0; i < netif->count; ++i)
    if (netif->preferred[i]) out[count++] = netif->ip6[i];
  return count;
}
'''

CASES = r'''
int main(void) {
  interfaces[0] = (esp_netif_t){.key="WIFI_STA_DEF", .name="sta", .index=7, .up=1};
  interfaces[1] = (esp_netif_t){.key="ETH_DEF", .name="eth", .index=8, .up=1};
  default_netif = &interfaces[0];
  const uint8_t deprecated[16] = {0x20, 1, 0x0d, 0xb8};
  const uint8_t local[16] = {0xfe, 0x80};
  const uint8_t preferred[16] = {0xfd, 0x42};
  memcpy(interfaces[0].ip6[0].addr, deprecated, 16);
  memcpy(interfaces[0].ip6[1].addr, local, 16);
  memcpy(interfaces[0].ip6[2].addr, preferred, 16);
  interfaces[0].preferred[1] = interfaces[0].preferred[2] = 1;
  interfaces[0].count = 3;
  h2_pal_net_addr_t address;
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_OK);
  assert(memcmp(address.ip, preferred, 16) == 0 && address.scope_id == 0u);

  interfaces[0].count = 1;  // Only a deprecated global address remains.
  memset(&address, 0xa5, sizeof(address));
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_ERR_UNAVAILABLE);
  const h2_pal_net_addr_t zero = {0};
  assert(memcmp(&address, &zero, sizeof(address)) == 0);

  interfaces[0].count = 2;  // A preferred link-local address is still usable with scope.
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_OK);
  assert(memcmp(address.ip, local, 16) == 0 && address.scope_id == 7u);

  interfaces[0].count = 1;
  memcpy(interfaces[1].ip6[0].addr, preferred, 16);
  interfaces[1].count = 1;
  interfaces[1].preferred[0] = 1;
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_OK);
  assert(memcmp(address.ip, preferred, 16) == 0);
  assert(esp_net_get_host_addr_family(NULL, "eth", H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_OK);
  interfaces[1].up = 0;
  assert(esp_net_get_host_addr_family(NULL, "eth", H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_ERR_UNAVAILABLE);

  interfaces[0].ip4.ip.addr = 0x04030201u;
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV4, &address) == H2_PAL_OK);
  assert(address.family == H2_PAL_NET_FAMILY_IPV4 &&
         memcmp(address.ip, &interfaces[0].ip4.ip.addr, 4) == 0);
  unsigned before = callbacks;
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_ANY, &address) == H2_PAL_ERR_INVALID_ARG);
  assert(callbacks == before);
  fail_tcpip = 1;
  memset(&address, 0xa5, sizeof(address));
  assert(esp_net_get_host_addr_family(NULL, NULL, H2_PAL_NET_FAMILY_IPV6, &address) == H2_PAL_ERR_IO);
  assert(memcmp(&address, &zero, sizeof(address)) == 0);
  puts("ESP host address: preferred/default/fallback/scoped/IPv4/TCPIP cases PASS");
  return 0;
}
'''


class ESPHostAddress(unittest.TestCase):
    def test_production_host_entry_rejects_deprecated_sources(self):
        source = (ROOT / "native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c").read_text()
        # This is the complete production context, selector, TCP/IP callback
        # and public entry. Only SDK types and inventory side effects are fake.
        start = source.index("typedef struct esp_net_host_addr_context {")
        end = source.index("static int esp_net_get_host_addr(", start)
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-esp-host-address-") as temporary:
            test = Path(temporary) / "host.c"
            binary = Path(temporary) / "host"
            test.write_text(SDK + source[start:end] + CASES)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(ROOT / "libs/pal/include"), str(test), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
