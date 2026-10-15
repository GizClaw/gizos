#include "h2_desktop_platform.h"
#include "h2_desktop_resource_stats_internal.h"

#include <cstring>
#include <new>

struct h2_desktop_firmware_info {
  h2_pal_firmware_info_t image;
  h2_pal_firmware_info_api_t api;
};

namespace {
h2_pal_result_t get_current(void *user, h2_pal_firmware_info_t *out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  std::memset(out, 0, sizeof(*out));
  if (user == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  const auto *owner = static_cast<const h2_desktop_firmware_info_t *>(user);
  *out = owner->image;
  return H2_PAL_OK;
}
const h2_pal_firmware_info_vtable_t vtable = {get_current};
} // namespace

extern "C" h2_pal_result_t
h2_desktop_firmware_info_create(const char *embedded_version,
                                h2_desktop_firmware_info_t **out) {
  if (out != nullptr)
    *out = nullptr;
  if (embedded_version == nullptr || embedded_version[0] == '\0' ||
      out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  size_t length = 0u;
  while (length < H2_PAL_FIRMWARE_VERSION_MAX &&
         embedded_version[length] != '\0')
    ++length;
  if (length == H2_PAL_FIRMWARE_VERSION_MAX)
    return H2_PAL_ERR_TRUNCATED;
  auto *owner = new (std::nothrow) h2_desktop_firmware_info_t();
  if (owner == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  std::memcpy(owner->image.version, embedded_version, length + 1u);
  owner->api = {owner, &vtable};
  h2_desktop_resource_acquire(h2_desktop_resource_kind::firmware_info);
  *out = owner;
  return H2_PAL_OK;
}

extern "C" const h2_pal_firmware_info_api_t *
h2_desktop_firmware_info_api(h2_desktop_firmware_info_t *owner) {
  return owner == nullptr ? nullptr : &owner->api;
}

extern "C" void
h2_desktop_firmware_info_destroy(h2_desktop_firmware_info_t *owner) {
  if (owner != nullptr) {
    delete owner;
    h2_desktop_resource_release(h2_desktop_resource_kind::firmware_info);
  }
}
