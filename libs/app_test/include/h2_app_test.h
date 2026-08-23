#ifndef H2_APP_TEST_H
#define H2_APP_TEST_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum App identifier bytes, excluding the terminating null. */
#define H2_APP_TEST_APP_ID_MAX 31u
/** Maximum versioned initial-state payload bytes. */
#define H2_APP_TEST_FIXTURE_MAX 4096u
/** Maximum named values in one immutable snapshot. */
#define H2_APP_TEST_PROBE_COUNT_MAX 352u
/** Maximum probe-name bytes, excluding the terminating null. */
#define H2_APP_TEST_PROBE_NAME_MAX 63u
/** Maximum string-probe bytes, excluding the terminating null. */
#define H2_APP_TEST_STRING_MAX 255u
/** Maximum aggregate string bytes in one snapshot, including nulls. */
#define H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX 512u
/** Maximum aggregate probe-name bytes in one snapshot, including nulls. */
#define H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX 9216u

/** Framework-owned copy of one public Runtime event. */
typedef struct h2_app_test_event {
  /** Runtime event kind. */
  h2_runtime_event_kind_t kind;
  /** Runtime component family. */
  h2_runtime_component_t component;
  /** Runtime component identifier. */
  h2_runtime_component_id_t component_id;
  /** Source event monotonic timestamp. */
  h2_runtime_timestamp_ms_t timestamp_ms;
  /** Number of valid payload bytes. */
  size_t payload_size;
  /** Bounded, framework-owned event payload. */
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} h2_app_test_event_t;

/** Versioned, framework-owned App initial-state fixture. */
typedef struct h2_app_test_fixture {
  /** App-defined nonzero schema identifier. */
  uint32_t schema;
  /** Number of valid fixture bytes. */
  size_t size;
  /** Bounded, framework-owned fixture payload. */
  uint8_t data[H2_APP_TEST_FIXTURE_MAX];
} h2_app_test_fixture_t;

/** Maximum copied Runtime component-state bytes in one semantic operation. */
#define H2_APP_TEST_COMPONENT_STATE_MAX 256u

typedef enum h2_app_test_operation_kind {
  H2_APP_TEST_OPERATION_RUN = 1,
  H2_APP_TEST_OPERATION_EVENT,
  H2_APP_TEST_OPERATION_COMPONENT_STATE,
  H2_APP_TEST_OPERATION_BUTTON_DOWN,
  H2_APP_TEST_OPERATION_BUTTON_UP,
  H2_APP_TEST_OPERATION_BUTTON_ACTION,
} h2_app_test_operation_kind_t;

/**
 * Driver-owned semantic Runtime operation.
 *
 * This object is never passed to an App adapter. The Memory driver translates
 * it into Runtime test-control calls before invoking the App production step.
 */
typedef struct h2_app_test_operation {
  h2_app_test_operation_kind_t kind;
  union {
    h2_app_test_event_t event;
    struct {
      h2_runtime_component_id_t component_id;
      size_t size;
      uint8_t data[H2_APP_TEST_COMPONENT_STATE_MAX];
    } component_state;
    struct {
      h2_runtime_component_id_t component_id;
      h2_runtime_timestamp_ms_t pressed_at_ms;
      h2_runtime_timestamp_ms_t released_at_ms;
      /** BUTTON_ACTION only: one-based consecutive-click count. */
      uint16_t click_count;
    } button;
  } data;
} h2_app_test_operation_t;

/** Supported named snapshot value types. */
typedef enum h2_app_test_value_kind {
  H2_APP_TEST_VALUE_BOOL = 1,
  H2_APP_TEST_VALUE_I32,
  H2_APP_TEST_VALUE_U32,
  H2_APP_TEST_VALUE_I64,
  H2_APP_TEST_VALUE_U64,
  H2_APP_TEST_VALUE_STRING,
} h2_app_test_value_kind_t;

/** One typed named value in an App Test snapshot. */
typedef struct h2_app_test_probe {
  /** Active member of value. */
  h2_app_test_value_kind_t kind;
  /** Offset of the null-terminated name in snapshot name storage. */
  uint16_t name_offset;
  /** Probe-name bytes, excluding the terminating null. */
  uint16_t name_length;
  /** Bounded probe value storage. */
  union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    struct {
      uint16_t offset;
      uint16_t length;
    } string;
  } value;
} h2_app_test_probe_t;

/** Immutable result of one completed driver step. */
typedef struct h2_app_test_snapshot {
  /** Generation that produced this snapshot. */
  uint32_t generation;
  /** Production App loop-step result recorded by the driver. */
  h2_pal_result_t step_result;
  /** Number of valid probes. */
  size_t probe_count;
  /** Number of valid bytes in name_storage. */
  size_t name_size;
  /** Number of valid bytes in string_storage. */
  size_t string_size;
  /** Bounded named probe storage. */
  h2_app_test_probe_t probes[H2_APP_TEST_PROBE_COUNT_MAX];
  /** Aggregate storage referenced by probe name offsets. */
  char name_storage[H2_APP_TEST_SNAPSHOT_NAME_BYTES_MAX];
  /** Aggregate storage referenced by string probes. */
  char string_storage[H2_APP_TEST_SNAPSHOT_STRING_BYTES_MAX];
} h2_app_test_snapshot_t;

/** Validated writer passed only to an App snapshot callback. */
typedef struct h2_app_test_snapshot_writer {
  /** Snapshot being populated. */
  h2_app_test_snapshot_t *snapshot;
} h2_app_test_snapshot_writer_t;

/** App-owned integration callbacks used by every execution driver. */
typedef struct h2_app_test_app_vtable {
  /** Initialize App state and production subjects from a versioned fixture. */
  h2_pal_result_t (*reset)(void *user,
                           const h2_app_test_fixture_t *fixture);
  /** Return the initialized Runtime borrowed by the active session. */
  h2_runtime_t *(*runtime)(void *user);
  /** Execute the shared production App loop step without test input. */
  h2_pal_result_t (*run_step)(void *user, uint32_t timeout_ms);
  /** Write App-state and production-subject probes. */
  h2_pal_result_t (*snapshot)(void *user,
                              h2_app_test_snapshot_writer_t *writer);
  /** Release partial or complete App state; must be idempotent. */
  void (*stop)(void *user);
} h2_app_test_app_vtable_t;

/** One App adapter registered with a driver. */
typedef struct h2_app_test_app {
  /** Stable identifier selected by a scenario. */
  const char *app_id;
  /** App-owned callback context. */
  void *user;
  /** App-owned integration callbacks. */
  const h2_app_test_app_vtable_t *vtable;
} h2_app_test_app_t;

typedef struct h2_app_test_driver h2_app_test_driver_t;
typedef struct h2_app_test_session h2_app_test_session_t;

/** Execution-driver operations used by the driver-neutral session API. */
typedef struct h2_app_test_driver_vtable {
  /** Open one App fixture. */
  h2_pal_result_t (*open)(void *user, const char *app_id,
                          const h2_app_test_fixture_t *fixture);
  /** Execute one semantic Runtime operation and snapshot its barrier. */
  h2_pal_result_t (*execute)(void *user,
                             const h2_app_test_operation_t *operation,
                             uint32_t generation, uint32_t timeout_ms,
                             h2_app_test_snapshot_t *snapshot);
  /** Close a partial or complete driver session; must be idempotent. */
  void (*close)(void *user);
} h2_app_test_driver_vtable_t;

/** Session state embedded in a driver. */
struct h2_app_test_session {
  /** Owning driver while open. */
  h2_app_test_driver_t *driver;
  /** Last successfully completed generation. */
  uint32_t last_generation;
  /** Whether this session currently owns an App instance. */
  bool open;
};

/** Driver object initialized by a concrete execution implementation. */
struct h2_app_test_driver {
  /** Driver implementation context. */
  void *user;
  /** Concrete execution operations. */
  const h2_app_test_driver_vtable_t *vtable;
  /** The single session supported by the initial contract. */
  h2_app_test_session_t session;
  /** Aligned private storage reserved for built-in drivers. */
  uintptr_t implementation_storage[16];
};

/**
 * Copy one public Runtime event into bounded framework storage.
 *
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
h2_pal_result_t h2_app_test_event_init(
    h2_app_test_event_t *event, h2_runtime_event_kind_t kind,
    h2_runtime_component_t component, h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms, const void *payload,
    size_t payload_size);

/**
 * Copy one versioned App initial-state fixture into bounded storage.
 *
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
h2_pal_result_t h2_app_test_fixture_init(h2_app_test_fixture_t *fixture,
                                         uint32_t schema, const void *data,
                                         size_t size);

/** Open the one session owned by driver. */
h2_pal_result_t
h2_app_test_session_open(h2_app_test_driver_t *driver, const char *app_id,
                         const h2_app_test_fixture_t *fixture,
                         h2_app_test_session_t **session);

/** Inject one public Runtime event, run the production barrier and snapshot. */
h2_pal_result_t h2_app_test_session_emit_event(
    h2_app_test_session_t *session, const h2_app_test_event_t *event,
    uint32_t timeout_ms, h2_app_test_snapshot_t *snapshot);

/** Set one Runtime-owned component state and run the production barrier. */
h2_pal_result_t h2_app_test_session_set_component_state(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id, const void *state,
    size_t state_size, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot);

h2_pal_result_t h2_app_test_session_button_down(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot);

h2_pal_result_t h2_app_test_session_button_up(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot);

/**
 * Inject one BUTTON_ACTION with the Runtime payload the App receives in
 * production; `click_count` is the one-based consecutive-click count and
 * must be non-zero.
 */
h2_pal_result_t h2_app_test_session_button_action(
    h2_app_test_session_t *session,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms, uint16_t click_count,
    uint32_t timeout_ms, h2_app_test_snapshot_t *snapshot);

/** Run a production barrier without injecting Runtime input. */
h2_pal_result_t h2_app_test_session_run(
    h2_app_test_session_t *session, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot);

/** Close a session; null and repeated close are no-ops. */
void h2_app_test_session_close(h2_app_test_session_t *session);

/** Write one uniquely named boolean probe. */
h2_pal_result_t
h2_app_test_snapshot_write_bool(h2_app_test_snapshot_writer_t *writer,
                                const char *name, bool value);
/** Write one uniquely named signed 32-bit probe. */
h2_pal_result_t
h2_app_test_snapshot_write_i32(h2_app_test_snapshot_writer_t *writer,
                               const char *name, int32_t value);
/** Write one uniquely named unsigned 32-bit probe. */
h2_pal_result_t
h2_app_test_snapshot_write_u32(h2_app_test_snapshot_writer_t *writer,
                               const char *name, uint32_t value);
/** Write one uniquely named signed 64-bit probe. */
h2_pal_result_t
h2_app_test_snapshot_write_i64(h2_app_test_snapshot_writer_t *writer,
                               const char *name, int64_t value);
/** Write one uniquely named unsigned 64-bit probe. */
h2_pal_result_t
h2_app_test_snapshot_write_u64(h2_app_test_snapshot_writer_t *writer,
                               const char *name, uint64_t value);
/** Copy one uniquely named bounded string probe. */
h2_pal_result_t
h2_app_test_snapshot_write_string(h2_app_test_snapshot_writer_t *writer,
                                  const char *name, const char *value);

/** Read a boolean probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_bool(const h2_app_test_snapshot_t *snapshot,
                              const char *name, bool *value);
/** Read a signed 32-bit probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_i32(const h2_app_test_snapshot_t *snapshot,
                             const char *name, int32_t *value);
/** Read an unsigned 32-bit probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_u32(const h2_app_test_snapshot_t *snapshot,
                             const char *name, uint32_t *value);
/** Read a signed 64-bit probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_i64(const h2_app_test_snapshot_t *snapshot,
                             const char *name, int64_t *value);
/** Read an unsigned 64-bit probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_u64(const h2_app_test_snapshot_t *snapshot,
                             const char *name, uint64_t *value);
/** Borrow a string probe by exact name and type. */
h2_pal_result_t
h2_app_test_snapshot_get_string(const h2_app_test_snapshot_t *snapshot,
                                const char *name, const char **value);

#ifdef __cplusplus
}
#endif

#endif
