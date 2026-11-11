#include "web_buttons.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

typedef struct collected {
  h2_pal_periph_info_t periphs[8];
  h2_runtime_component_mapping_entry_t mappings[8];
  size_t count;
} collected_t;

static h2_pal_result_t on_periph(void* user, const h2_pal_periph_info_t* info) {
  collected_t* c = user;
  if (c->count >= 8u) return H2_PAL_ERR_NO_SPACE;
  c->periphs[c->count++] = *info;
  return H2_PAL_OK;
}

static h2_pal_result_t on_mapping(
    void* user, const h2_runtime_component_mapping_entry_t* entry) {
  collected_t* c = user;
  if (c->count >= 8u) return H2_PAL_ERR_NO_SPACE;
  c->mappings[c->count++] = *entry;
  return H2_PAL_OK;
}

static h2_pal_result_t stop_early(void* user,
                                  const h2_pal_periph_info_t* info) {
  (void)user;
  (void)info;
  return H2_PAL_ERR_IO;
}

static void test_names(void) {
  CHECK(h2_g99_web_component("home") == H2_G99_COMPONENT_HOME);
  CHECK(h2_g99_web_component("screen") == H2_G99_COMPONENT_SCREEN);
  CHECK(h2_g99_web_component("up") == H2_G99_COMPONENT_UP);
  CHECK(h2_g99_web_component("down") == H2_G99_COMPONENT_DOWN);
  CHECK(h2_g99_web_component("ok") == H2_G99_COMPONENT_OK);
  CHECK(h2_g99_web_component("back") == H2_RUNTIME_COMPONENT_ID_NONE);
  CHECK(h2_g99_web_component("") == H2_RUNTIME_COMPONENT_ID_NONE);
  CHECK(h2_g99_web_component(NULL) == H2_RUNTIME_COMPONENT_ID_NONE);
}

static void test_periphs_and_mapping_agree(void) {
  const h2_pal_periph_api_t* periph = h2_g99_web_periph_api();
  const h2_runtime_component_mapper_t* mapper = h2_g99_web_component_mapper();
  collected_t periphs = {0};
  CHECK(periph->vtable->list(periph->user, H2_PAL_PERIPH_TYPE_ANY, on_periph,
                             &periphs) == H2_PAL_OK);
  CHECK(periphs.count == 5u);
  for (size_t i = 0u; i < periphs.count; ++i) {
    const h2_pal_periph_info_t* info = &periphs.periphs[i];
    CHECK(info->type == H2_PAL_PERIPH_TYPE_SINGLE_BUTTON);
    CHECK(info->payload_size == sizeof(h2_pal_periph_single_button_payload_t));
    const h2_pal_periph_single_button_payload_t* payload = info->payload;
    CHECK(payload != NULL &&
          payload->delivery == H2_PAL_BUTTON_DELIVERY_PUSH_EDGE);
    h2_pal_periph_info_t again;
    CHECK(periph->vtable->get(periph->user, info->id, &again) == H2_PAL_OK);
    CHECK(again.id == info->id && strcmp(again.name, info->name) == 0);
    /* Each Button's periph maps back to the component its name selects. */
    h2_pal_periph_id_t mapped = 0u;
    CHECK(mapper->vtable->get_periph_id(mapper->user,
                                        h2_g99_web_component(info->name),
                                        &mapped) == H2_PAL_OK);
    CHECK(mapped == info->id);
  }
  collected_t none = {0};
  CHECK(periph->vtable->list(periph->user, H2_PAL_PERIPH_TYPE_BATTERY,
                             on_periph, &none) == H2_PAL_OK);
  CHECK(none.count == 0u);
  CHECK(periph->vtable->list(periph->user, H2_PAL_PERIPH_TYPE_ANY, stop_early,
                             NULL) == H2_PAL_ERR_IO);
  h2_pal_periph_info_t info;
  CHECK(periph->vtable->get(periph->user, 1u, &info) == H2_PAL_ERR_NOT_FOUND);
  CHECK(periph->vtable->get(periph->user, 1001u + 5u, &info) ==
        H2_PAL_ERR_NOT_FOUND);
  collected_t buttons = {0};
  CHECK(mapper->vtable->list(mapper->user, H2_RUNTIME_COMPONENT_BUTTON,
                             on_mapping, &buttons) == H2_PAL_OK);
  CHECK(buttons.count == 5u);
  collected_t batteries = {0};
  CHECK(mapper->vtable->list(mapper->user, H2_RUNTIME_COMPONENT_BATTERY,
                             on_mapping, &batteries) == H2_PAL_OK);
  CHECK(batteries.count == 0u);
  h2_pal_periph_id_t id = 0u;
  CHECK(mapper->vtable->get_periph_id(mapper->user, H2_G99_COMPONENT_BATTERY,
                                      &id) == H2_PAL_ERR_NOT_FOUND);
}

static void test_edge_arguments(void) {
  CHECK(h2_g99_web_button_edge(NULL, H2_G99_COMPONENT_OK, 1) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_g99_web_button_edge((h2_runtime_t*)&failures,
                               H2_G99_COMPONENT_BATTERY,
                               1) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_names();
  test_periphs_and_mapping_agree();
  test_edge_arguments();
  if (failures != 0) {
    fprintf(stderr, "%d web button check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
