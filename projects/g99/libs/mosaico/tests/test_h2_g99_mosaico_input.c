#include "h2_g99.h"
#include "h2_g99_mosaico_input.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

/* A stand-in for the board: AI, BOOT and the fuel gauge. */
static const h2_pal_periph_id_t kBoardIds[] = {101u, 102u, 301u};

static h2_pal_result_t board_list(void* user, h2_pal_periph_type_t filter,
                                  h2_pal_periph_cb_t callback,
                                  void* callback_user) {
  (void)user;
  for (size_t i = 0u; i < 3u; ++i) {
    h2_pal_periph_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = kBoardIds[i];
    info.type =
        i < 2u ? H2_PAL_PERIPH_TYPE_SINGLE_BUTTON : H2_PAL_PERIPH_TYPE_BATTERY;
    if (filter != H2_PAL_PERIPH_TYPE_ANY && filter != info.type) continue;
    const h2_pal_result_t rc = callback(callback_user, &info);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t board_get(void* user, h2_pal_periph_id_t id,
                                 h2_pal_periph_info_t* out) {
  (void)user;
  for (size_t i = 0u; i < 3u; ++i)
    if (kBoardIds[i] == id) {
      memset(out, 0, sizeof(*out));
      out->id = id;
      return H2_PAL_OK;
    }
  return H2_PAL_ERR_NOT_FOUND;
}

static const h2_pal_periph_vtable_t kBoardVtable = {board_list, board_get};
static const h2_pal_periph_api_t kBoard = {NULL, &kBoardVtable};

static const h2_g99_mosaico_input_config_t kProduct = {
    .home_periph_id = 101u,
    .screen_periph_id = 102u,
    .battery_periph_id = 301u,
};

static const h2_g99_mosaico_input_config_t kE2e = {
    .home_periph_id = 101u,
    .screen_periph_id = 102u,
    .battery_periph_id = 301u,
    .virtual_first_id = 901u,
    .virtual_keys = true,
};

static h2_pal_result_t count_periph(void* user,
                                    const h2_pal_periph_info_t* info) {
  (void)info;
  ++*(int*)user;
  return H2_PAL_OK;
}

static h2_pal_result_t count_mapping(
    void* user, const h2_runtime_component_mapping_entry_t* entry) {
  (void)entry;
  ++*(int*)user;
  return H2_PAL_OK;
}

static h2_pal_periph_id_t periph_of(const h2_g99_mosaico_input_t* input,
                                    h2_runtime_component_id_t component) {
  h2_pal_periph_id_t id = 0u;
  if (input->mapper.vtable->get_periph_id(input->mapper.user, component, &id) !=
      H2_PAL_OK)
    return 0u;
  return id;
}

static void test_product(void) {
  h2_g99_mosaico_input_t input;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &kProduct) == H2_PAL_OK);
  CHECK(periph_of(&input, H2_G99_COMPONENT_HOME) == 101u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_SCREEN) == 102u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_BATTERY) == 301u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_UP) == 0u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_OK) == 0u);
  int periphs = 0;
  CHECK(h2_pal_periph_list(&input.periph, H2_PAL_PERIPH_TYPE_ANY, count_periph,
                           &periphs) == H2_PAL_OK);
  CHECK(periphs == 3);
  int mappings = 0;
  CHECK(input.mapper.vtable->list(input.mapper.user, H2_RUNTIME_COMPONENT_NONE,
                                  count_mapping, &mappings) == H2_PAL_OK);
  CHECK(mappings == 3);
  h2_pal_periph_info_t info;
  CHECK(h2_pal_periph_get(&input.periph, 901u, &info) == H2_PAL_ERR_NOT_FOUND);
}

static void test_e2e(void) {
  h2_g99_mosaico_input_t input;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &kE2e) == H2_PAL_OK);
  CHECK(periph_of(&input, H2_G99_COMPONENT_HOME) == 101u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_UP) == 901u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_DOWN) == 902u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_OK) == 903u);
  int buttons = 0;
  CHECK(h2_pal_periph_list(&input.periph, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
                           count_periph, &buttons) == H2_PAL_OK);
  CHECK(buttons == 5);
  int batteries = 0;
  CHECK(h2_pal_periph_list(&input.periph, H2_PAL_PERIPH_TYPE_BATTERY,
                           count_periph, &batteries) == H2_PAL_OK);
  CHECK(batteries == 1);
  int button_mappings = 0;
  CHECK(input.mapper.vtable->list(input.mapper.user,
                                  H2_RUNTIME_COMPONENT_BUTTON, count_mapping,
                                  &button_mappings) == H2_PAL_OK);
  CHECK(button_mappings == 5);
  h2_pal_periph_info_t info;
  CHECK(h2_pal_periph_get(&input.periph, 902u, &info) == H2_PAL_OK);
  CHECK(info.type == H2_PAL_PERIPH_TYPE_SINGLE_BUTTON);
  CHECK(strcmp(info.name, "e2e_down") == 0);
  const h2_pal_periph_single_button_payload_t* payload = info.payload;
  CHECK(payload != NULL &&
        payload->delivery == H2_PAL_BUTTON_DELIVERY_PUSH_EDGE);
  CHECK(h2_pal_periph_get(&input.periph, 102u, &info) == H2_PAL_OK);
  CHECK(h2_g99_mosaico_input_init(&input, NULL, &kE2e) ==
        H2_PAL_ERR_INVALID_ARG);
}

static void test_config(void) {
  h2_g99_mosaico_input_t input;
  h2_g99_mosaico_input_config_t config = kProduct;
  config.home_periph_id = 102u;
  config.screen_periph_id = 101u;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &config) == H2_PAL_OK);
  CHECK(periph_of(&input, H2_G99_COMPONENT_HOME) == 102u);
  CHECK(periph_of(&input, H2_G99_COMPONENT_SCREEN) == 101u);
  config.battery_periph_id = 999u;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &config) ==
        H2_PAL_ERR_NOT_FOUND);
  config = kE2e;
  config.virtual_first_id = 100u;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &config) ==
        H2_PAL_ERR_INVALID_ARG);
  config.virtual_first_id = UINT32_MAX;
  CHECK(h2_g99_mosaico_input_init(&input, &kBoard, &config) ==
        H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_product();
  test_e2e();
  test_config();
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
