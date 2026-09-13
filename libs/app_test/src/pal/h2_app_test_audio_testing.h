#ifndef H2_APP_TEST_AUDIO_TESTING_H
#define H2_APP_TEST_AUDIO_TESTING_H

#include "h2_app_test_audio.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only seam, compiled only into the testonly `testing_audio_testing`
 * library (H2_APP_TEST_AUDIO_TESTING). Hold returns false when the fixture
 * lock is already held. The caller owns a successful hold and must release it
 * before calling any decorator operation that should succeed; a failed test
 * assertion aborts the process, so no hold outlives its test. */
bool h2_app_test_audio_test_hold_fixture_lock(h2_app_test_audio_t *audio);
void h2_app_test_audio_test_release_fixture_lock(h2_app_test_audio_t *audio);

#ifdef __cplusplus
}
#endif

#endif
