#ifndef H2_APP_TEST_CASE_H
#define H2_APP_TEST_CASE_H

#include "h2_app_test.h"
#include "h2_runtime_input_button.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct h2_app_test_case {
  h2_app_test_driver_t *driver;
  h2_app_test_session_t *session;
  h2_app_test_snapshot_t snapshot;
  h2_pal_result_t result;
  h2_runtime_timestamp_ms_t button_pressed_at[32];
  bool button_pressed[32];
} h2_app_test_case_t;

/** Driver-neutral scenario function registered by an App test suite. */
typedef h2_pal_result_t (*h2_app_test_case_fn)(h2_app_test_driver_t *driver);

/** Define a scenario that always closes its session before returning. */
#define H2_APP_TEST_CASE(name)                                                 \
  static void name##_body(h2_app_test_case_t *test);                           \
  h2_pal_result_t name(h2_app_test_driver_t *driver) {                         \
    h2_app_test_case_t test = {.driver = driver, .result = H2_PAL_OK};         \
    name##_body(&test);                                                        \
    h2_app_test_session_close(test.session);                                   \
    return test.result;                                                        \
  }                                                                            \
  static void name##_body(h2_app_test_case_t *test)

#define H2_APP_TEST_REQUIRE(test_, expression_)                                \
  do {                                                                         \
    h2_pal_result_t h2_app_test_rc_ = (expression_);                           \
    if (h2_app_test_rc_ != H2_PAL_OK) {                                        \
      fprintf(stderr, "REQUIRE failed: %s:%d: %s returned %" PRId32 "\n",     \
              __FILE__, __LINE__, #expression_, (int32_t)h2_app_test_rc_);     \
      (test_)->result = h2_app_test_rc_;                                       \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_OPEN(test_, app_id_, schema_, initial_)                    \
  do {                                                                         \
    h2_app_test_fixture_t h2_app_test_fixture_;                                \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_fixture_init(&h2_app_test_fixture_, (schema_),    \
                                           (initial_), sizeof(*(initial_))));  \
    H2_APP_TEST_REQUIRE((test_),                                               \
                        h2_app_test_session_open((test_)->driver, (app_id_),   \
                                                 &h2_app_test_fixture_,        \
                                                 &(test_)->session));          \
  } while (0)

#define H2_APP_TEST_EXPECT_STEP_RESULT(test_, expected_)                       \
  do {                                                                         \
    const h2_pal_result_t h2_app_test_expected_result_ = (expected_);          \
    if ((test_)->snapshot.step_result != h2_app_test_expected_result_) {       \
      fprintf(stderr,                                                          \
              "EXPECT_STEP_RESULT expected=%" PRId32 " actual=%" PRId32      \
              "\n",                                                           \
              (int32_t)h2_app_test_expected_result_,                           \
              (int32_t)(test_)->snapshot.step_result);                         \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_STEP_EVENT(test_, event_, now_ms_)                         \
  do {                                                                         \
    (void)(now_ms_);                                                           \
    H2_APP_TEST_REQUIRE((test_), h2_app_test_session_emit_event(               \
                                     (test_)->session, &(event_), 0u,          \
                                     &(test_)->snapshot));                     \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_STEP_EVENT_RESULT(test_, event_, now_ms_,                  \
                                      expected_result_)                        \
  do {                                                                         \
    (void)(now_ms_);                                                           \
    H2_APP_TEST_REQUIRE((test_), h2_app_test_session_emit_event(               \
                                     (test_)->session, &(event_), 0u,          \
                                     &(test_)->snapshot));                     \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), (expected_result_));               \
  } while (0)

#define H2_APP_TEST_SET_COMPONENT_STATE(test_, component_id_, state_)          \
  do {                                                                         \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_session_set_component_state(                     \
                     (test_)->session, (component_id_), (state_),              \
                     sizeof(*(state_)), 0u, &(test_)->snapshot));              \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_RUN(test_)                                                 \
  do {                                                                         \
    H2_APP_TEST_REQUIRE((test_), h2_app_test_session_run(                      \
                                     (test_)->session, 0u,                     \
                                     &(test_)->snapshot));                     \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_BUTTON_ACTION(test_, component_id_, pressed_, released_)    \
  H2_APP_TEST_BUTTON_ACTION_COUNT((test_), (component_id_), (pressed_),        \
                                  (released_), 1u)

/* Inject one BUTTON_ACTION carrying a consecutive-click count. */
#define H2_APP_TEST_BUTTON_ACTION_COUNT(test_, component_id_, pressed_,        \
                                        released_, click_count_)               \
  do {                                                                         \
    H2_APP_TEST_REQUIRE((test_),                                               \
                        h2_app_test_session_button_action(                      \
                            (test_)->session, (component_id_), (pressed_),     \
                            (released_), (click_count_), 0u,                   \
                            &(test_)->snapshot));                              \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_BUTTON_ACTION_RESULT(test_, component_id_, pressed_,        \
                                        released_, expected_result_)           \
  do {                                                                         \
    H2_APP_TEST_REQUIRE((test_),                                               \
                        h2_app_test_session_button_action(                      \
                            (test_)->session, (component_id_), (pressed_),     \
                            (released_), 1u, 0u, &(test_)->snapshot));         \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), (expected_result_));               \
  } while (0)

#define H2_APP_TEST_BUTTON_DOWN(test_, component_id_, timestamp_)              \
  do {                                                                         \
    const size_t h2_app_test_button_index_ = (size_t)(component_id_);          \
    if (h2_app_test_button_index_ < 32u) {                                     \
      (test_)->button_pressed_at[h2_app_test_button_index_] = (timestamp_);    \
      (test_)->button_pressed[h2_app_test_button_index_] = true;               \
    }                                                                          \
    H2_APP_TEST_REQUIRE((test_), h2_app_test_session_button_down(              \
                                     (test_)->session, (component_id_),        \
                                     (timestamp_), 0u, &(test_)->snapshot));   \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_BUTTON_UP(test_, component_id_, timestamp_)                \
  do {                                                                         \
    const size_t h2_app_test_button_index_ = (size_t)(component_id_);          \
    h2_runtime_timestamp_ms_t h2_app_test_pressed_at_ = (timestamp_);          \
    if (h2_app_test_button_index_ < 32u &&                                     \
        (test_)->button_pressed[h2_app_test_button_index_]) {                  \
      h2_app_test_pressed_at_ =                                                \
          (test_)->button_pressed_at[h2_app_test_button_index_];               \
      (test_)->button_pressed[h2_app_test_button_index_] = false;              \
    }                                                                          \
    H2_APP_TEST_REQUIRE((test_), h2_app_test_session_button_up(                \
                                     (test_)->session, (component_id_),        \
                                     h2_app_test_pressed_at_, (timestamp_),    \
                                     0u, &(test_)->snapshot));                 \
    H2_APP_TEST_EXPECT_STEP_RESULT((test_), H2_PAL_OK);                        \
  } while (0)

#define H2_APP_TEST_EXPECT_I32(test_, name_, expected_)                        \
  do {                                                                         \
    int32_t h2_app_test_actual_;                                               \
    const int32_t h2_app_test_expected_ = (int32_t)(expected_);                \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_i32(&(test_)->snapshot, (name_),     \
                                              &h2_app_test_actual_));          \
    if (h2_app_test_actual_ != h2_app_test_expected_) {                        \
      fprintf(stderr, "EXPECT_I32 probe=%s expected=%" PRId32                  \
                      " actual=%" PRId32 "\n",                                \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_EXPECT_U32(test_, name_, expected_)                        \
  do {                                                                         \
    uint32_t h2_app_test_actual_;                                              \
    const uint32_t h2_app_test_expected_ = (uint32_t)(expected_);              \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_u32(&(test_)->snapshot, (name_),     \
                                              &h2_app_test_actual_));          \
    if (h2_app_test_actual_ != h2_app_test_expected_) {                        \
      fprintf(stderr, "EXPECT_U32 probe=%s expected=%" PRIu32                  \
                      " actual=%" PRIu32 "\n",                                \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_EXPECT_BOOL(test_, name_, expected_)                       \
  do {                                                                         \
    bool h2_app_test_actual_;                                                  \
    const bool h2_app_test_expected_ = (bool)(expected_);                      \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_bool(&(test_)->snapshot, (name_),    \
                                               &h2_app_test_actual_));         \
    if (h2_app_test_actual_ != h2_app_test_expected_) {                        \
      fprintf(stderr, "EXPECT_BOOL probe=%s expected=%d actual=%d\n",          \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_EXPECT_I64(test_, name_, expected_)                        \
  do {                                                                         \
    int64_t h2_app_test_actual_;                                               \
    const int64_t h2_app_test_expected_ = (int64_t)(expected_);                \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_i64(&(test_)->snapshot, (name_),     \
                                              &h2_app_test_actual_));          \
    if (h2_app_test_actual_ != h2_app_test_expected_) {                        \
      fprintf(stderr, "EXPECT_I64 probe=%s expected=%" PRId64                  \
                      " actual=%" PRId64 "\n",                                \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_EXPECT_U64(test_, name_, expected_)                        \
  do {                                                                         \
    uint64_t h2_app_test_actual_;                                              \
    const uint64_t h2_app_test_expected_ = (uint64_t)(expected_);              \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_u64(&(test_)->snapshot, (name_),     \
                                              &h2_app_test_actual_));          \
    if (h2_app_test_actual_ != h2_app_test_expected_) {                        \
      fprintf(stderr, "EXPECT_U64 probe=%s expected=%" PRIu64                  \
                      " actual=%" PRIu64 "\n",                                \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define H2_APP_TEST_EXPECT_STRING(test_, name_, expected_)                     \
  do {                                                                         \
    const char *h2_app_test_actual_;                                           \
    const char *h2_app_test_expected_ = (expected_);                           \
    H2_APP_TEST_REQUIRE(                                                       \
        (test_), h2_app_test_snapshot_get_string(&(test_)->snapshot, (name_),  \
                                                 &h2_app_test_actual_));       \
    if (strcmp(h2_app_test_actual_, h2_app_test_expected_) != 0) {             \
      fprintf(stderr, "EXPECT_STRING probe=%s expected=%s actual=%s\n",        \
              (name_), h2_app_test_expected_, h2_app_test_actual_);            \
      (test_)->result = H2_PAL_ERR_INVALID_STATE;                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#endif
