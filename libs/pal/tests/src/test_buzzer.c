#include "h2/pal/hal/h2_pal_buzzer.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>

typedef struct buzzer_fixture {
    unsigned int calls;
    h2_pal_buzzer_id_t id;
    uint32_t frequency_hz;
    uint8_t volume_percent;
} buzzer_fixture_t;

static h2_pal_result_t fixture_get_info(
    void *user,
    h2_pal_buzzer_id_t id,
    h2_pal_buzzer_info_t *out_info) {
    buzzer_fixture_t *fixture = user;
    fixture->calls++;
    fixture->id = id;
    *out_info = (h2_pal_buzzer_info_t){
        .id = id,
        .min_frequency_hz = 100u,
        .max_frequency_hz = 10000u,
        .supports_volume = 1u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_start(
    void *user,
    h2_pal_buzzer_id_t id,
    uint32_t frequency_hz,
    uint8_t volume_percent) {
    buzzer_fixture_t *fixture = user;
    fixture->calls++;
    fixture->id = id;
    fixture->frequency_hz = frequency_hz;
    fixture->volume_percent = volume_percent;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_stop(
    void *user,
    h2_pal_buzzer_id_t id) {
    buzzer_fixture_t *fixture = user;
    fixture->calls++;
    fixture->id = id;
    return H2_PAL_OK;
}

int main(void) {
    buzzer_fixture_t fixture = {0};
    const h2_pal_buzzer_vtable_t vtable = {
        .get_info = fixture_get_info,
        .start = fixture_start,
        .stop = fixture_stop,
    };
    const h2_pal_buzzer_api_t api = {
        .user = &fixture,
        .vtable = &vtable,
    };
    h2_pal_buzzer_info_t info = {0};

    assert(h2_pal_buzzer_get_info(NULL, 7u, &info) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_buzzer_get_info(&api, 7u, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_buzzer_start(&api, 7u, 0u, 50u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_buzzer_start(&api, 7u, 440u, 101u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(fixture.calls == 0u);

    assert(h2_pal_buzzer_get_info(&api, 7u, &info) == H2_PAL_OK);
    assert(info.id == 7u);
    assert(info.min_frequency_hz == 100u);
    assert(info.max_frequency_hz == 10000u);
    assert(info.supports_volume == 1u);
    assert(h2_pal_buzzer_start(&api, 7u, 440u, 75u) == H2_PAL_OK);
    assert(fixture.id == 7u);
    assert(fixture.frequency_hz == 440u);
    assert(fixture.volume_percent == 75u);
    assert(h2_pal_buzzer_stop(&api, 7u) == H2_PAL_OK);
    assert(fixture.calls == 3u);

    const h2_pal_buzzer_api_t *unsupported =
        h2_pal_unsupported_buzzer_api();
    assert(unsupported != NULL && unsupported->vtable != NULL);
    assert(h2_pal_buzzer_get_info(unsupported, 7u, &info) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_buzzer_start(unsupported, 7u, 440u, 75u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_buzzer_stop(unsupported, 7u) ==
           H2_PAL_ERR_UNSUPPORTED);
    return 0;
}
