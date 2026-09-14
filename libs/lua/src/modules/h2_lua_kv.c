#include "h2_lua_storage_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

/* The persisted types are independent of host byte order and C alignment. */
_Static_assert(sizeof(lua_Integer) == 8 && LUA_MAXINTEGER == INT64_MAX,
               "KV requires 64-bit Lua integers");
_Static_assert(sizeof(lua_Number) == 8 && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024 && FLT_RADIX == 2,
               "KV requires binary64 Lua numbers");

#define KV_HEADER_SIZE 16u
#define KV_OVERHEAD 20u
#define KV_RECORD_HEADER 6u

typedef enum kv_operation {
  KV_GET,
  KV_SET,
  KV_REMOVE,
  KV_EXISTS,
  KV_KEYS
} kv_operation_t;
typedef enum kv_type {
  KV_STRING = 1,
  KV_BOOLEAN,
  KV_INTEGER,
  KV_FLOAT
} kv_type_t;

typedef struct kv_record {
  const uint8_t *key;
  const uint8_t *value;
  size_t key_size;
  size_t value_size;
  unsigned type;
} kv_record_t;

static uint64_t kv_load(const uint8_t *p, size_t n) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < n; ++i)
    value |= (uint64_t)p[i] << (8u * i);
  return value;
}

static void kv_store(uint8_t *p, uint64_t value, size_t n) {
  for (size_t i = 0u; i < n; ++i)
    p[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t kv_crc(const uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0u; i < n; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0u; bit < 8u; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
  }
  return crc ^ UINT32_MAX;
}

static int kv_key_valid(const uint8_t *key, size_t n) {
  if (n == 0u || n > H2_LUA_STORAGE_NAME_MAX || key[0] == '.')
    return 0;
  for (size_t i = 0u; i < n; ++i) {
    uint8_t c = key[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == '.'))
      return 0;
  }
  return 1;
}

static int kv_compare(const kv_record_t *a, const kv_record_t *b) {
  size_t n = a->key_size < b->key_size ? a->key_size : b->key_size;
  int cmp = memcmp(a->key, b->key, n);
  return cmp != 0 ? cmp
                  : (a->key_size > b->key_size) - (a->key_size < b->key_size);
}

/* Only used after validation (or after checking the record's available bytes).
 */
static size_t kv_record(const uint8_t *p, kv_record_t *r) {
  r->key_size = p[0];
  r->type = p[1];
  r->value_size = (size_t)kv_load(p + 2u, 4u);
  r->key = p + KV_RECORD_HEADER;
  r->value = r->key + r->key_size;
  return KV_RECORD_HEADER + r->key_size + r->value_size;
}

static const char *kv_validate(const uint8_t *data, size_t size,
                               uint32_t *count) {
  kv_record_t previous = {0};
  size_t offset = KV_HEADER_SIZE;
  *count = 0u;
  if (size == 0u)
    return NULL;
  if (size < 6u || memcmp(data, "GZKV", 4u) != 0)
    return "corrupt data";
  if (kv_load(data + 4u, 2u) != 1u)
    return "unsupported version";
  if (size < KV_OVERHEAD || kv_load(data + 6u, 2u) != 0u ||
      kv_load(data + 12u, 4u) != size - KV_OVERHEAD ||
      kv_load(data + size - 4u, 4u) != kv_crc(data, size - 4u))
    return "corrupt data";
  *count = (uint32_t)kv_load(data + 8u, 4u);
  if (*count > H2_LUA_KV_MAX_KEYS)
    return "corrupt data";
  for (uint32_t i = 0u; i < *count; ++i) {
    kv_record_t r;
    size_t remaining = size - 4u - offset;
    size_t key_size, value_size;
    if (remaining < KV_RECORD_HEADER)
      return "corrupt data";
    key_size = data[offset];
    value_size = (size_t)kv_load(data + offset + 2u, 4u);
    remaining -= KV_RECORD_HEADER;
    if (key_size > remaining || value_size > remaining - key_size)
      return "corrupt data";
    offset += kv_record(data + offset, &r);
    if (!kv_key_valid(r.key, r.key_size) ||
        (i != 0u && kv_compare(&previous, &r) >= 0))
      return "corrupt data";
    switch (r.type) {
    case KV_STRING:
      break;
    case KV_BOOLEAN:
      if (r.value_size != 1u || r.value[0] > 1u)
        return "corrupt data";
      break;
    case KV_INTEGER:
      if (r.value_size != 8u)
        return "corrupt data";
      break;
    case KV_FLOAT: {
      uint64_t bits;
      lua_Number number;
      if (r.value_size != 8u)
        return "corrupt data";
      bits = kv_load(r.value, 8u);
      memcpy(&number, &bits, sizeof(number));
      if (!isfinite(number))
        return "corrupt data";
      break;
    }
    default:
      return "corrupt data";
    }
    previous = r;
  }
  return offset == size - 4u ? NULL : "corrupt data";
}

static const char *kv_storage_error(storage_status_t status) {
  switch (status) {
  case STORAGE_OK:
    return NULL;
  case STORAGE_UNAVAILABLE:
    return "unavailable";
  case STORAGE_QUOTA_EXCEEDED:
    return "quota exceeded";
  case STORAGE_TOO_MANY_FILES:
    return "too many files";
  case STORAGE_NO_SPACE:
    return "no space";
  case STORAGE_BUSY:
  case STORAGE_CHANGED:
    return "busy";
  case STORAGE_CORRUPT:
    return "corrupt data";
  default:
    return "io error";
  }
}

static int kv_failure(lua_State *state, const char *reason) {
  lua_pushnil(state);
  lua_pushfstring(state, "kv: %s", reason);
  return 2;
}

static size_t kv_encode_record(uint8_t *p, const kv_record_t *r) {
  p[0] = (uint8_t)r->key_size;
  p[1] = (uint8_t)r->type;
  kv_store(p + 2u, r->value_size, 4u);
  memcpy(p + KV_RECORD_HEADER, r->key, r->key_size);
  memcpy(p + KV_RECORD_HEADER + r->key_size, r->value, r->value_size);
  return KV_RECORD_HEADER + r->key_size + r->value_size;
}

/* The old snapshot is validated. Scan it twice, first calculating the exact
 * size; no partially produced output can be committed on an error. */
static const char *kv_mutate(h2_lua_job_t *job, kv_operation_t op,
                             const kv_record_t *input, const uint8_t *old,
                             size_t old_size, uint32_t count, uint8_t *out,
                             size_t capacity, int *changed, int *retry) {
  size_t offset = KV_HEADER_SIZE, removed = 0u;
  size_t total = old_size == 0u ? KV_OVERHEAD : old_size;
  int found = 0, inserted = 0;
  uint32_t old_count = count;
  for (uint32_t i = 0u; i < count; ++i) {
    kv_record_t r;
    size_t n = kv_record(old + offset, &r);
    if (kv_compare(&r, input) == 0) {
      found = 1;
      removed = n;
    }
    offset += n;
  }
  *changed = found;
  if (op == KV_REMOVE && !found)
    return NULL;
  if (op == KV_SET && !found && count == H2_LUA_KV_MAX_KEYS)
    return "too many keys";
  total -= removed;
  if (op == KV_SET) {
    size_t added = KV_RECORD_HEADER + input->key_size + input->value_size;
    if (SIZE_MAX - total < added)
      return "quota exceeded";
    total += added;
    if (!found)
      ++count;
  } else {
    --count;
  }
  if (count == 0u)
    return kv_storage_error(h2_lua_storage_kv_commit_locked(job, NULL, 0u));
  if (total > job->host->config.storage.app_quota_bytes ||
      total - KV_OVERHEAD > UINT32_MAX)
    return "quota exceeded";
  if (total > capacity) {
    *retry = 1;
    return NULL;
  }
  offset = KV_HEADER_SIZE;
  size_t written = KV_HEADER_SIZE;
  for (uint32_t i = 0u; i < old_count; ++i) {
    kv_record_t r;
    size_t n = kv_record(old + offset, &r);
    int cmp = kv_compare(&r, input);
    if (op == KV_SET && !inserted && cmp >= 0) {
      written += kv_encode_record(out + written, input);
      inserted = 1;
    }
    if (cmp != 0) {
      memcpy(out + written, old + offset, n);
      written += n;
    }
    offset += n;
  }
  if (op == KV_SET && !inserted)
    written += kv_encode_record(out + written, input);
  memcpy(out, "GZKV", 4u);
  kv_store(out + 4u, 1u, 2u);
  kv_store(out + 6u, 0u, 2u);
  kv_store(out + 8u, count, 4u);
  kv_store(out + 12u, written - KV_HEADER_SIZE, 4u);
  kv_store(out + written, kv_crc(out, written), 4u);
  return kv_storage_error(
      h2_lua_storage_kv_commit_locked(job, out, written + 4u));
}

static int kv_result(lua_State *state, kv_operation_t op, const uint8_t *data,
                     uint32_t count, const kv_record_t *input) {
  size_t offset = KV_HEADER_SIZE;
  if (op == KV_KEYS)
    lua_createtable(state, (int)count, 0);
  for (uint32_t i = 0u; i < count; ++i) {
    kv_record_t r;
    offset += kv_record(data + offset, &r);
    if (op == KV_KEYS) {
      lua_pushlstring(state, (const char *)r.key, r.key_size);
      lua_rawseti(state, -2, i + 1u);
    } else if (kv_compare(&r, input) == 0) {
      if (op == KV_EXISTS)
        lua_pushboolean(state, 1);
      else if (r.type == KV_STRING)
        lua_pushlstring(state, (const char *)r.value, r.value_size);
      else if (r.type == KV_BOOLEAN)
        lua_pushboolean(state, r.value[0]);
      else if (r.type == KV_INTEGER) {
        uint64_t bits = kv_load(r.value, 8u);
        lua_Integer value = bits <= INT64_MAX
                                ? (lua_Integer)bits
                                : -1 - (lua_Integer)(UINT64_MAX - bits);
        lua_pushinteger(state, value);
      } else {
        uint64_t bits = kv_load(r.value, 8u);
        lua_Number value;
        memcpy(&value, &bits, sizeof(value));
        lua_pushnumber(state, value);
      }
      return 1;
    }
  }
  if (op == KV_GET)
    lua_pushnil(state);
  else if (op == KV_EXISTS)
    lua_pushboolean(state, 0);
  return 1;
}

static int kv_call(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  kv_operation_t op = (kv_operation_t)lua_tointeger(state, lua_upvalueindex(2));
  kv_record_t input = {0};
  uint8_t scalar[8] = {0};
  const char *error = NULL;
  int base = lua_gettop(state);
  if (op != KV_KEYS) {
    luaL_checktype(state, 1, LUA_TSTRING);
    input.key = (const uint8_t *)lua_tolstring(state, 1, &input.key_size);
    if (!kv_key_valid(input.key, input.key_size))
      error = "invalid key";
  }
  if (op == KV_SET) {
    switch (lua_type(state, 2)) {
    case LUA_TSTRING:
      input.type = KV_STRING;
      input.value = (const uint8_t *)lua_tolstring(state, 2, &input.value_size);
      break;
    case LUA_TBOOLEAN:
      input.type = KV_BOOLEAN;
      scalar[0] = (uint8_t)lua_toboolean(state, 2);
      input.value_size = 1u;
      break;
    case LUA_TNUMBER:
      input.value_size = 8u;
      if (lua_isinteger(state, 2)) {
        input.type = KV_INTEGER;
        kv_store(scalar, (uint64_t)lua_tointeger(state, 2), 8u);
      } else {
        lua_Number value = lua_tonumber(state, 2);
        uint64_t bits;
        input.type = KV_FLOAT;
        if (!isfinite(value))
          error = "invalid value";
        memcpy(&bits, &value, sizeof(bits));
        kv_store(scalar, bits, 8u);
      }
      break;
    default:
      return luaL_argerror(state, 2, "expected string, boolean or number");
    }
    if (input.type != KV_STRING)
      input.value = scalar;
  }
  if (error != NULL)
    return kv_failure(state, error);
  if (!h2_lua_storage_available(job))
    return kv_failure(state, "unavailable");
  if (input.value_size > UINT32_MAX ||
      input.value_size > SIZE_MAX - KV_RECORD_HEADER - input.key_size)
    return kv_failure(state, "quota exceeded");
  for (int attempt = 0; attempt < STORAGE_READ_ATTEMPTS; ++attempt) {
    uint64_t size64 = 0u, current64 = 0u;
    size_t size, capacity = 0u;
    uint8_t *old, *out = NULL;
    uint32_t count = 0u;
    int changed = 0, retry = 0;
    storage_status_t status = h2_lua_storage_lock(job);
    if (status == STORAGE_OK) {
      status = h2_lua_storage_kv_size_locked(job, &size64);
      h2_lua_storage_unlock(job);
    }
    if (status != STORAGE_OK)
      return kv_failure(state, kv_storage_error(status));
    if (size64 > job->host->config.storage.app_quota_bytes ||
        size64 > SIZE_MAX ||
        (size64 > KV_OVERHEAD && size64 - KV_OVERHEAD > UINT32_MAX))
      return kv_failure(state, "corrupt data");
    size = (size_t)size64;
    old = lua_newuserdatauv(state, size == 0u ? 1u : size, 0);
    if (op == KV_SET || op == KV_REMOVE) {
      capacity = size;
      if (op == KV_SET) {
        size_t added = KV_RECORD_HEADER + input.key_size + input.value_size;
        capacity = size == 0u ? KV_OVERHEAD : size;
        capacity = SIZE_MAX - capacity < added ? SIZE_MAX : capacity + added;
        if (capacity > job->host->config.storage.app_quota_bytes)
          capacity = job->host->config.storage.app_quota_bytes;
      }
      if ((uint64_t)capacity > (uint64_t)UINT32_MAX + KV_OVERHEAD)
        capacity = (size_t)((uint64_t)UINT32_MAX + KV_OVERHEAD);
      out = lua_newuserdatauv(state, capacity == 0u ? 1u : capacity, 0);
    }
    status = h2_lua_storage_lock(job);
    if (status == STORAGE_OK) {
      status = h2_lua_storage_kv_size_locked(job, &current64);
      if (status == STORAGE_OK && current64 > size64)
        status = STORAGE_CHANGED;
      if (status == STORAGE_OK)
        status = h2_lua_storage_kv_read_locked(job, old, (size_t)current64);
      if (status == STORAGE_OK) {
        error = kv_validate(old, (size_t)current64, &count);
        if (error == NULL && (op == KV_SET || op == KV_REMOVE))
          error = kv_mutate(job, op, &input, old, (size_t)current64, count, out,
                            capacity, &changed, &retry);
      }
      h2_lua_storage_unlock(job);
    }
    if (status == STORAGE_CHANGED || retry) {
      lua_settop(state, base);
      continue;
    }
    if (status != STORAGE_OK)
      return kv_failure(state, kv_storage_error(status));
    if (error != NULL)
      return kv_failure(state, error);
    if (op == KV_SET || op == KV_REMOVE) {
      lua_pushboolean(state, op == KV_SET || changed);
      return 1;
    }
    return kv_result(state, op, old, count, &input);
  }
  return kv_failure(state, "busy");
}

int h2_lua_open_kv(lua_State *state) {
  static const char *const names[] = {"get", "set", "remove", "exists", "keys"};
  void *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_createtable(state, 0, 5);
  for (int i = 0; i < 5; ++i) {
    lua_pushlightuserdata(state, job);
    lua_pushinteger(state, i);
    lua_pushcclosure(state, kv_call, 2);
    lua_setfield(state, -2, names[i]);
  }
  return 1;
}
