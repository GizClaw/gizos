#ifndef H2_JIELI_WL82_GATT_H
#define H2_JIELI_WL82_GATT_H

#include "h2/pal/hal/h2_pal_ble.h"
#include <string.h>

#define H2_JIELI_GATT_MAX_CHARACTERISTICS 16u
#define H2_JIELI_GATT_DATABASE_SIZE 2048u

typedef struct h2_jieli_gatt_database {
  uint8_t bytes[H2_JIELI_GATT_DATABASE_SIZE];
  size_t size;
  size_t count;
  h2_pal_ble_gatt_characteristic_t
      characteristics[H2_JIELI_GATT_MAX_CHARACTERISTICS];
  uint16_t value_handles[H2_JIELI_GATT_MAX_CHARACTERISTICS];
  uint16_t cccd_handles[H2_JIELI_GATT_MAX_CHARACTERISTICS];
  uint16_t service_handles[4];
} h2_jieli_gatt_database_t;

static inline void h2_jieli_gatt_u16(uint8_t* out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
}
static inline int h2_jieli_gatt_uuid_valid(const h2_pal_ble_uuid_t* uuid) {
  return uuid->data != NULL && (uuid->len == 2u || uuid->len == 16u);
}
static inline int h2_jieli_gatt_attribute(h2_jieli_gatt_database_t* db,
                                          uint16_t handle, uint16_t flags,
                                          const uint8_t* uuid, size_t uuid_size,
                                          const void* value,
                                          size_t value_size) {
  const size_t size = 6u + uuid_size + value_size;
  if (size > UINT16_MAX || size + 2u > sizeof(db->bytes) - db->size)
    return H2_PAL_ERR_NO_SPACE;
  uint8_t* out = db->bytes + db->size;
  h2_jieli_gatt_u16(out, (uint16_t)size);
  h2_jieli_gatt_u16(out + 2u, flags | (uuid_size == 16u ? 0x0200u : 0u));
  h2_jieli_gatt_u16(out + 4u, handle);
  memcpy(out + 6u, uuid, uuid_size);
  if (value_size != 0u) memcpy(out + 6u + uuid_size, value, value_size);
  db->size += size;
  return H2_PAL_OK;
}

/* Build the pinned SDK's ATT byte format from the public registration schema.
 * The standard GAP prefix keeps its required Device Name handle 3. All product
 * UUIDs, properties, callbacks, values, and handles come from the caller. */
static inline int h2_jieli_gatt_build(h2_jieli_gatt_database_t* db,
                                      const h2_pal_ble_gatt_service_t* services,
                                      size_t count) {
  if (db == NULL || services == NULL || count == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (count > 4u) return H2_PAL_ERR_NO_SPACE;
  static const uint8_t gap[] = {
      0x0a, 0,    2, 0, 1, 0, 0,    0x28, 0, 0x18, 0x0d, 0, 2, 0, 2,    0,
      3,    0x28, 2, 3, 0, 0, 0x2a, 8,    0, 2,    1,    3, 0, 0, 0x2a,
  };
  memset(db, 0, sizeof(*db));
  memcpy(db->bytes, gap, sizeof(gap));
  db->size = sizeof(gap);
  uint16_t handle = 4u;
  for (size_t i = 0u; i < count; ++i) {
    const h2_pal_ble_gatt_service_t* service = &services[i];
    if (!h2_jieli_gatt_uuid_valid(&service->uuid) ||
        (service->characteristic_count != 0u &&
         service->characteristics == NULL))
      return H2_PAL_ERR_INVALID_ARG;
    if (service->characteristic_count >
        H2_JIELI_GATT_MAX_CHARACTERISTICS - db->count)
      return H2_PAL_ERR_NO_SPACE;
    uint8_t service_type[2] = {service->primary ? 0x00u : 0x01u, 0x28u};
    db->service_handles[i] = handle;
    int rc = h2_jieli_gatt_attribute(db, handle++, 2u, service_type, 2u,
                                     service->uuid.data, service->uuid.len);
    if (rc != H2_PAL_OK) return rc;
    for (size_t j = 0u; j < service->characteristic_count; ++j) {
      const h2_pal_ble_gatt_characteristic_t* characteristic =
          &service->characteristics[j];
      const uint32_t properties = characteristic->properties;
      if (!h2_jieli_gatt_uuid_valid(&characteristic->uuid) ||
          (properties & ~UINT32_C(31)) != 0u || properties == 0u ||
          characteristic->max_value_len > 512u ||
          characteristic->initial_value_len > characteristic->max_value_len ||
          (characteristic->initial_value_len != 0u &&
           characteristic->initial_value == NULL))
        return H2_PAL_ERR_INVALID_ARG;
      /* No silent weakening of security permissions unsupported by this port.
       */
      if ((characteristic->permissions & ~UINT32_C(3)) != 0u)
        return H2_PAL_ERR_UNSUPPORTED;
      uint8_t att_properties =
          ((properties & H2_PAL_BLE_GATT_PROPERTY_READ) ? 2u : 0u) |
          ((properties & H2_PAL_BLE_GATT_PROPERTY_WRITE) ? 8u : 0u) |
          ((properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) ? 4u : 0u) |
          ((properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) ? 16u : 0u) |
          ((properties & H2_PAL_BLE_GATT_PROPERTY_INDICATE) ? 32u : 0u);
      uint8_t declaration[19] = {att_properties};
      h2_jieli_gatt_u16(declaration + 1u, handle + 1u);
      memcpy(declaration + 3u, characteristic->uuid.data,
             characteristic->uuid.len);
      static const uint8_t declaration_type[2] = {3u, 0x28u};
      rc = h2_jieli_gatt_attribute(db, handle++, 2u, declaration_type, 2u,
                                   declaration, 3u + characteristic->uuid.len);
      if (rc != H2_PAL_OK) return rc;
      db->value_handles[db->count] = handle;
      uint16_t flags = att_properties;
      if (characteristic->read != NULL || characteristic->write != NULL)
        flags |= 0x0100u;
      rc = h2_jieli_gatt_attribute(
          db, handle++, flags, characteristic->uuid.data,
          characteristic->uuid.len, characteristic->initial_value,
          characteristic->initial_value_len);
      if (rc != H2_PAL_OK) return rc;
      if ((properties & (H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
                         H2_PAL_BLE_GATT_PROPERTY_INDICATE)) != 0u) {
        static const uint8_t cccd_type[2] = {2u, 0x29u};
        static const uint8_t cccd[2] = {0u, 0u};
        db->cccd_handles[db->count] = handle;
        rc = h2_jieli_gatt_attribute(db, handle++, 0x010au, cccd_type, 2u, cccd,
                                     2u);
        if (rc != H2_PAL_OK) return rc;
      }
      db->characteristics[db->count++] = *characteristic;
    }
  }
  db->bytes[db->size] = db->bytes[db->size + 1u] = 0u;
  db->size += 2u;
  return H2_PAL_OK;
}
#endif
