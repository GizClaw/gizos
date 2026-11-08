#include "h2_mfg.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)

static int save(void* user, const h2_loader_mfg_summary_t* summary) {
  *(h2_loader_mfg_summary_t*)user = *summary;
  return H2_PAL_OK;
}

int main(void) {
  const h2_mfg_profile_t* profile = &h2_mfg_board_profile;
  assert(strcmp(profile->board, STRINGIFY(H2_EXPECTED_BOARD)) == 0);
  assert(profile->revision != 0);
  assert(profile->total == (H2_EXPECT_BLE ? 13u : 12u));
  assert(profile->button_count == 8u && profile->buttons[0] == 102u);
  assert(profile->button_group == (H2_EXPECT_BLE ? 100u : 0u));
  assert(profile->button_group_mask == (H2_EXPECT_BLE ? 0x7fu : 0u));
  assert(profile->led == 601u && profile->battery == 401u &&
         profile->motor == 701u);
  for (unsigned i = 0; i < profile->button_count; ++i) {
    assert(profile->buttons[i] >= 101u && profile->buttons[i] <= 108u);
    assert(profile->button_names[i] != NULL && profile->button_names[i][0]);
    for (unsigned j = 0; j < i; ++j)
      assert(profile->buttons[j] != profile->buttons[i]);
  }

  unsigned checks = 0;
  for (unsigned i = 0; i < profile->total; ++i) {
    unsigned bit = 1u << profile->steps[i].check;
    assert((checks & bit) == 0);
    checks |= bit;
    if (profile->steps[i].check == H2_MFG_AEC ||
        profile->steps[i].check == H2_MFG_NFC)
      assert(!profile->steps[i].automatic);
  }
  unsigned expected =
      (1u << H2_MFG_STORAGE) | (1u << H2_MFG_DISPLAY) | (1u << H2_MFG_BUTTONS) |
      (1u << H2_MFG_SPEAKER) | (1u << H2_MFG_MIC) | (1u << H2_MFG_WIFI) |
      (1u << H2_MFG_LED) | (1u << H2_MFG_MODEM) | (1u << H2_MFG_BATTERY) |
      (1u << H2_MFG_VIBRATION) | (1u << H2_MFG_NFC) | (1u << H2_MFG_AEC);
  if (H2_EXPECT_BLE) expected |= 1u << H2_MFG_BLE;
  assert(checks == expected);

  h2_loader_mfg_summary_t durable = {.total = profile->total};
  h2_mfg_model_t model;
  assert(h2_mfg_model_init(&model, profile, &durable, 1u, save, &durable) == 0);
  assert(!h2_mfg_model_complete(&model));
  const h2_mfg_step_t* step = &profile->steps[model.current];
  assert(h2_mfg_model_action(&model, "confirm", step->name, model.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  for (unsigned i = 0; i < profile->total; ++i) {
    assert(!h2_mfg_model_complete(&model));
    step = &profile->steps[model.current];
    model.ready = 1;
    assert(h2_mfg_model_action(&model, "confirm", step->name, model.ticket) ==
           H2_PAL_OK);
  }
  assert(h2_mfg_model_complete(&model));
  assert(h2_mfg_model_action(&model, "retry", "nfc", model.ticket) ==
         H2_PAL_OK);
  assert(!h2_mfg_model_complete(&model) && !model.ready);
  return 0;
}
