#include "h2_app_test.h"

#include <string.h>

static size_t bounded_length(const char *value, size_t limit) {
  if (value == NULL) {
    return limit;
  }
  size_t length = 0u;
  while (length < limit && value[length] != '\0') {
    ++length;
  }
  return length;
}

static bool valid_name(const char *name) {
  return name != NULL && name[0] != '\0' &&
         bounded_length(name, H2_APP_TEST_PROBE_NAME_MAX + 1u) <=
             H2_APP_TEST_PROBE_NAME_MAX;
}

static h2_pal_result_t
snapshot_probe_name(const h2_app_test_snapshot_t *snapshot,
                    const h2_app_test_probe_t *probe, const char **name) {
  if (snapshot == NULL || probe == NULL || name == NULL ||
      snapshot->name_size > H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  const size_t offset = probe->name_offset;
  const size_t length = probe->name_length;
  if (length == 0u || length > H2_APP_TEST_PROBE_NAME_MAX ||
      offset > snapshot->name_size || length >= snapshot->name_size - offset ||
      snapshot->name_storage[offset + length] != '\0' ||
      bounded_length(&snapshot->name_storage[offset], length + 1u) != length) {
    return H2_PAL_ERR_FORMAT;
  }
  *name = &snapshot->name_storage[offset];
  return H2_PAL_OK;
}

static h2_pal_result_t validate_snapshot(const h2_app_test_snapshot_t *snapshot,
                                         uint32_t generation) {
  if (snapshot->generation != generation) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (snapshot->probe_count > H2_APP_TEST_PROBE_COUNT_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  if (snapshot->name_size > H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  if (snapshot->string_size > H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  for (size_t index = 0u; index < snapshot->probe_count; ++index) {
    const h2_app_test_probe_t *probe = &snapshot->probes[index];
    const char *probe_name = NULL;
    if (snapshot_probe_name(snapshot, probe, &probe_name) != H2_PAL_OK ||
        probe->kind < H2_APP_TEST_VALUE_BOOL ||
        probe->kind > H2_APP_TEST_VALUE_STRING) {
      return H2_PAL_ERR_FORMAT;
    }
    if (probe->kind == H2_APP_TEST_VALUE_STRING) {
      const size_t offset = probe->value.string.offset;
      const size_t length = probe->value.string.length;
      if (length > H2_APP_TEST_STRING_MAX || offset > snapshot->string_size ||
          length >= snapshot->string_size - offset ||
          snapshot->string_storage[offset + length] != '\0') {
        return H2_PAL_ERR_FORMAT;
      }
    }
    for (size_t previous = 0u; previous < index; ++previous) {
      const char *previous_name = NULL;
      if (snapshot_probe_name(snapshot, &snapshot->probes[previous],
                              &previous_name) != H2_PAL_OK ||
          strcmp(previous_name, probe_name) == 0) {
        return H2_PAL_ERR_FORMAT;
      }
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_app_test_event_init(
    h2_app_test_event_t *event, h2_runtime_event_kind_t kind,
    h2_runtime_component_t component, h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms, const void *payload,
    size_t payload_size) {
  if (event == NULL || kind == H2_RUNTIME_EVENT_NONE ||
      payload_size > H2_RUNTIME_EVENT_PAYLOAD_MAX ||
      (payload_size != 0u && payload == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(event, 0, sizeof(*event));
  event->kind = kind;
  event->component = component;
  event->component_id = component_id;
  event->timestamp_ms = timestamp_ms;
  event->payload_size = payload_size;
  if (payload_size != 0u) {
    memcpy(event->payload, payload, payload_size);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_app_test_fixture_init(h2_app_test_fixture_t *fixture,
                                         uint32_t schema, const void *data,
                                         size_t size) {
  if (fixture == NULL || schema == 0u || size > H2_APP_TEST_FIXTURE_MAX ||
      (size != 0u && data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(fixture, 0, sizeof(*fixture));
  fixture->schema = schema;
  fixture->size = size;
  if (size != 0u) {
    memcpy(fixture->data, data, size);
  }
  return H2_PAL_OK;
}

h2_pal_result_t
h2_app_test_session_open(h2_app_test_driver_t *driver, const char *app_id,
                         const h2_app_test_fixture_t *fixture,
                         h2_app_test_session_t **session) {
  if (driver == NULL || driver->vtable == NULL ||
      driver->vtable->open == NULL || driver->vtable->execute == NULL ||
      driver->vtable->close == NULL || app_id == NULL || app_id[0] == '\0' ||
      bounded_length(app_id, H2_APP_TEST_APP_ID_MAX + 1u) >
          H2_APP_TEST_APP_ID_MAX ||
      fixture == NULL || fixture->schema == 0u ||
      fixture->size > H2_APP_TEST_FIXTURE_MAX || session == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *session = NULL;
  if (driver->session.open) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t rc =
      driver->vtable->open(driver->user, app_id, fixture);
  if (rc != H2_PAL_OK) {
    driver->vtable->close(driver->user);
    return rc;
  }
  driver->session.driver = driver;
  driver->session.last_generation = 0u;
  driver->session.open = true;
  *session = &driver->session;
  return H2_PAL_OK;
}

static h2_pal_result_t session_execute(
    h2_app_test_session_t *session,
    const h2_app_test_operation_t *operation,
    uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  if (session == NULL || !session->open || session->driver == NULL ||
      operation == NULL || snapshot == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (session->last_generation == UINT32_MAX) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const uint32_t generation = session->last_generation + 1u;
  memset(snapshot, 0, sizeof(*snapshot));
  h2_pal_result_t rc = session->driver->vtable->execute(
      session->driver->user, operation, generation, timeout_ms, snapshot);
  if (rc == H2_PAL_OK) {
    rc = validate_snapshot(snapshot, generation);
    if (rc == H2_PAL_OK) {
      session->last_generation = generation;
    }
  }
  return rc;
}

h2_pal_result_t h2_app_test_session_emit_event(
    h2_app_test_session_t *session, const h2_app_test_event_t *event,
    uint32_t timeout_ms, h2_app_test_snapshot_t *snapshot) {
  if (event == NULL || event->kind == H2_RUNTIME_EVENT_NONE ||
      event->payload_size > H2_RUNTIME_EVENT_PAYLOAD_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_app_test_operation_t operation = {
      .kind = H2_APP_TEST_OPERATION_EVENT,
      .data.event = *event,
  };
  return session_execute(session, &operation, timeout_ms, snapshot);
}

h2_pal_result_t h2_app_test_session_set_component_state(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id, const void *state,
    size_t state_size, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  if (component_id == H2_RUNTIME_COMPONENT_ID_NONE || state == NULL ||
      state_size == 0u || state_size > H2_APP_TEST_COMPONENT_STATE_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_app_test_operation_t operation = {
      .kind = H2_APP_TEST_OPERATION_COMPONENT_STATE,
      .data.component_state =
          {
              .component_id = component_id,
              .size = state_size,
          },
  };
  memcpy(operation.data.component_state.data, state, state_size);
  return session_execute(session, &operation, timeout_ms, snapshot);
}

static h2_pal_result_t session_button(
    h2_app_test_session_t *session, h2_app_test_operation_kind_t kind,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms, uint16_t click_count,
    uint32_t timeout_ms, h2_app_test_snapshot_t *snapshot) {
  if (component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_app_test_operation_t operation = {
      .kind = kind,
      .data.button =
          {
              .component_id = component_id,
              .pressed_at_ms = pressed_at_ms,
              .released_at_ms = released_at_ms,
              .click_count = click_count,
          },
  };
  return session_execute(session, &operation, timeout_ms, snapshot);
}

h2_pal_result_t h2_app_test_session_button_down(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  return session_button(
      session, H2_APP_TEST_OPERATION_BUTTON_DOWN, component_id,
      pressed_at_ms, 0u, 0u, timeout_ms, snapshot);
}

h2_pal_result_t h2_app_test_session_button_up(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  if (released_at_ms < pressed_at_ms) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return session_button(
      session, H2_APP_TEST_OPERATION_BUTTON_UP, component_id,
      pressed_at_ms, released_at_ms, 0u, timeout_ms, snapshot);
}

h2_pal_result_t h2_app_test_session_button_action(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms, uint16_t click_count,
    uint32_t timeout_ms, h2_app_test_snapshot_t *snapshot) {
  if (released_at_ms < pressed_at_ms || click_count == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return session_button(
      session, H2_APP_TEST_OPERATION_BUTTON_ACTION, component_id,
      pressed_at_ms, released_at_ms, click_count, timeout_ms, snapshot);
}

h2_pal_result_t h2_app_test_session_run(
    h2_app_test_session_t *session, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  const h2_app_test_operation_t operation = {
      .kind = H2_APP_TEST_OPERATION_RUN,
  };
  return session_execute(session, &operation, timeout_ms, snapshot);
}

void h2_app_test_session_close(h2_app_test_session_t *session) {
  if (session == NULL || !session->open || session->driver == NULL) {
    return;
  }
  session->driver->vtable->close(session->driver->user);
  session->open = false;
  session->last_generation = 0u;
  session->driver = NULL;
}

static h2_pal_result_t write_probe(h2_app_test_snapshot_writer_t *writer,
                                   const char *name,
                                   h2_app_test_value_kind_t kind,
                                   h2_app_test_probe_t **probe) {
  if (writer == NULL || writer->snapshot == NULL || !valid_name(name) ||
      probe == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_app_test_snapshot_t *snapshot = writer->snapshot;
  if (snapshot->probe_count > H2_APP_TEST_PROBE_COUNT_MAX ||
      snapshot->name_size > H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  for (size_t index = 0u; index < snapshot->probe_count; ++index) {
    const char *probe_name = NULL;
    if (snapshot_probe_name(snapshot, &snapshot->probes[index], &probe_name) !=
        H2_PAL_OK) {
      return H2_PAL_ERR_FORMAT;
    }
    if (strcmp(probe_name, name) == 0) {
      return H2_PAL_ERR_FORMAT;
    }
  }
  if (snapshot->probe_count >= H2_APP_TEST_PROBE_COUNT_MAX) {
    return H2_PAL_ERR_FULL;
  }
  const size_t name_length = strlen(name);
  if (name_length + 1u >
      H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX - snapshot->name_size) {
    return H2_PAL_ERR_FULL;
  }
  *probe = &snapshot->probes[snapshot->probe_count++];
  memset(*probe, 0, sizeof(**probe));
  (*probe)->name_offset = (uint16_t)snapshot->name_size;
  (*probe)->name_length = (uint16_t)name_length;
  memcpy(&snapshot->name_storage[snapshot->name_size], name, name_length + 1u);
  snapshot->name_size += name_length + 1u;
  (*probe)->kind = kind;
  return H2_PAL_OK;
}

#define H2_APP_TEST_DEFINE_WRITE(name_, kind_, member_, type_)                 \
  h2_pal_result_t name_(h2_app_test_snapshot_writer_t *writer,                 \
                        const char *name, type_ value) {                       \
    h2_app_test_probe_t *probe = NULL;                                         \
    h2_pal_result_t rc = write_probe(writer, name, kind_, &probe);             \
    if (rc == H2_PAL_OK) {                                                     \
      probe->value.member_ = value;                                            \
    }                                                                          \
    return rc;                                                                 \
  }

H2_APP_TEST_DEFINE_WRITE(h2_app_test_snapshot_write_bool,
                         H2_APP_TEST_VALUE_BOOL, boolean, bool)
H2_APP_TEST_DEFINE_WRITE(h2_app_test_snapshot_write_i32, H2_APP_TEST_VALUE_I32,
                         i32, int32_t)
H2_APP_TEST_DEFINE_WRITE(h2_app_test_snapshot_write_u32, H2_APP_TEST_VALUE_U32,
                         u32, uint32_t)
H2_APP_TEST_DEFINE_WRITE(h2_app_test_snapshot_write_i64, H2_APP_TEST_VALUE_I64,
                         i64, int64_t)
H2_APP_TEST_DEFINE_WRITE(h2_app_test_snapshot_write_u64, H2_APP_TEST_VALUE_U64,
                         u64, uint64_t)

h2_pal_result_t
h2_app_test_snapshot_write_string(h2_app_test_snapshot_writer_t *writer,
                                  const char *name, const char *value) {
  const size_t length = bounded_length(value, H2_APP_TEST_STRING_MAX + 1u);
  if (value == NULL || length > H2_APP_TEST_STRING_MAX || writer == NULL ||
      writer->snapshot == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (writer->snapshot->string_size > H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  if (length + 1u >
      H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX - writer->snapshot->string_size) {
    return H2_PAL_ERR_FULL;
  }
  h2_app_test_probe_t *probe = NULL;
  h2_pal_result_t rc =
      write_probe(writer, name, H2_APP_TEST_VALUE_STRING, &probe);
  if (rc == H2_PAL_OK) {
    h2_app_test_snapshot_t *snapshot = writer->snapshot;
    probe->value.string.offset = (uint16_t)snapshot->string_size;
    probe->value.string.length = (uint16_t)length;
    memcpy(&snapshot->string_storage[snapshot->string_size], value,
           length + 1u);
    snapshot->string_size += length + 1u;
  }
  return rc;
}

static h2_pal_result_t find_probe(const h2_app_test_snapshot_t *snapshot,
                                  const char *name,
                                  h2_app_test_value_kind_t kind,
                                  const h2_app_test_probe_t **probe) {
  if (snapshot == NULL || !valid_name(name) || probe == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (snapshot->probe_count > H2_APP_TEST_PROBE_COUNT_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  for (size_t index = 0u; index < snapshot->probe_count; ++index) {
    const char *probe_name = NULL;
    if (snapshot_probe_name(snapshot, &snapshot->probes[index], &probe_name) !=
        H2_PAL_OK) {
      return H2_PAL_ERR_FORMAT;
    }
    if (strcmp(probe_name, name) == 0) {
      if (snapshot->probes[index].kind != kind) {
        return H2_PAL_ERR_FORMAT;
      }
      *probe = &snapshot->probes[index];
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

#define H2_APP_TEST_DEFINE_GET(name_, kind_, member_, type_)                   \
  h2_pal_result_t name_(const h2_app_test_snapshot_t *snapshot,                \
                        const char *name, type_ *value) {                      \
    if (value == NULL) {                                                       \
      return H2_PAL_ERR_INVALID_ARG;                                           \
    }                                                                          \
    const h2_app_test_probe_t *probe = NULL;                                   \
    h2_pal_result_t rc = find_probe(snapshot, name, kind_, &probe);            \
    if (rc == H2_PAL_OK) {                                                     \
      *value = probe->value.member_;                                           \
    }                                                                          \
    return rc;                                                                 \
  }

H2_APP_TEST_DEFINE_GET(h2_app_test_snapshot_get_bool, H2_APP_TEST_VALUE_BOOL,
                       boolean, bool)
H2_APP_TEST_DEFINE_GET(h2_app_test_snapshot_get_i32, H2_APP_TEST_VALUE_I32, i32,
                       int32_t)
H2_APP_TEST_DEFINE_GET(h2_app_test_snapshot_get_u32, H2_APP_TEST_VALUE_U32, u32,
                       uint32_t)
H2_APP_TEST_DEFINE_GET(h2_app_test_snapshot_get_i64, H2_APP_TEST_VALUE_I64, i64,
                       int64_t)
H2_APP_TEST_DEFINE_GET(h2_app_test_snapshot_get_u64, H2_APP_TEST_VALUE_U64, u64,
                       uint64_t)

h2_pal_result_t
h2_app_test_snapshot_get_string(const h2_app_test_snapshot_t *snapshot,
                                const char *name, const char **value) {
  if (value == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_app_test_probe_t *probe = NULL;
  h2_pal_result_t rc =
      find_probe(snapshot, name, H2_APP_TEST_VALUE_STRING, &probe);
  if (rc == H2_PAL_OK) {
    const size_t offset = probe->value.string.offset;
    const size_t length = probe->value.string.length;
    if (snapshot->string_size > H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX ||
        length > H2_APP_TEST_STRING_MAX || offset > snapshot->string_size ||
        length >= snapshot->string_size - offset ||
        snapshot->string_storage[offset + length] != '\0') {
      return H2_PAL_ERR_FORMAT;
    }
    *value = &snapshot->string_storage[offset];
  }
  return rc;
}
