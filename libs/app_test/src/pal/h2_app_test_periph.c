#include "h2_app_test_periph.h"
#include <string.h>
static h2_app_test_periph_entry_t *find(h2_app_test_periph_t *p,
                                        h2_pal_periph_id_t id) {
  if (p->count > H2_APP_TEST_PERIPH_MAX)
    return NULL;
  for (size_t i = 0; i < p->count; ++i)
    if (p->entries[i].info.id == id)
      return &p->entries[i];
  return NULL;
}
static int list(void *u, h2_pal_periph_type_t type, h2_pal_periph_cb_t cb,
                void *cu) {
  h2_app_test_periph_t *p = u;
  if (p->count > H2_APP_TEST_PERIPH_MAX)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0; i < p->count; ++i)
    if (type == H2_PAL_PERIPH_TYPE_ANY || type == p->entries[i].info.type) {
      int rc = cb(cu, &p->entries[i].info);
      if (rc)
        return rc;
    }
  return 0;
}
static int get(void *u, h2_pal_periph_id_t id, h2_pal_periph_info_t *out) {
  memset(out, 0, sizeof(*out));
  h2_app_test_periph_entry_t *e = find(u, id);
  if (!e)
    return H2_PAL_ERR_NOT_FOUND;
  *out = e->info;
  return 0;
}
static int button(void *u, h2_pal_periph_id_t id,
                  h2_pal_single_button_reading_t *out) {
  memset(out, 0, sizeof(*out));
  h2_app_test_periph_entry_t *e = find(u, id);
  if (!e || e->info.type != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_ERR_NOT_FOUND;
  if (!h2_pal_button_state_is_valid(e->button))
    return H2_PAL_ERR_FORMAT;
  int rc = h2_app_test_fault_take(&e->read);
  if (!rc)
    *out = (h2_pal_single_button_reading_t){id, e->button};
  return rc;
}
static int battery(void *u, h2_pal_periph_id_t id,
                   h2_pal_battery_reading_t *out) {
  memset(out, 0, sizeof(*out));
  h2_app_test_periph_entry_t *e = find(u, id);
  if (!e || e->info.type != H2_PAL_PERIPH_TYPE_BATTERY)
    return H2_PAL_ERR_NOT_FOUND;
  if (e->battery.percent_x100 > 10000u)
    return H2_PAL_ERR_FORMAT;
  int rc = h2_app_test_fault_take(&e->read);
  if (!rc) {
    *out = e->battery;
    out->id = id;
  }
  return rc;
}
static int temperature(void *u, h2_pal_periph_id_t id,
                       h2_pal_temperature_reading_t *out) {
  memset(out, 0, sizeof(*out));
  h2_app_test_periph_entry_t *e = find(u, id);
  if (!e || e->info.type != H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR)
    return H2_PAL_ERR_NOT_FOUND;
  int rc = h2_app_test_fault_take(&e->read);
  if (!rc) {
    *out = e->temperature;
    out->id = id;
  }
  return rc;
}
static int set_duty(void *u, h2_pal_periph_id_t id, uint16_t duty) {
  h2_app_test_periph_entry_t *e = find(u, id);
  if (duty > 10000u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!e || e->info.type != H2_PAL_PERIPH_TYPE_PWM_SWITCH)
    return H2_PAL_ERR_NOT_FOUND;
  e->last_duty_x100 = duty;
  int rc = h2_app_test_fault_take(&e->write);
  if (!rc)
    e->duty_x100 = duty;
  return rc;
}
static int get_duty(void *u, h2_pal_periph_id_t id, uint16_t *out) {
  *out = 0;
  h2_app_test_periph_entry_t *e = find(u, id);
  if (!e || e->info.type != H2_PAL_PERIPH_TYPE_PWM_SWITCH)
    return H2_PAL_ERR_NOT_FOUND;
  int rc = h2_app_test_fault_take(&e->read);
  if (!rc)
    *out = e->duty_x100;
  return rc;
}
static const h2_pal_periph_vtable_t vtable = {.list = list, .get = get};
static const h2_pal_button_vtable_t buttons = {.read_single_button = button};
static const h2_pal_input_vtable_t inputs = {.read_battery = battery,
                                             .read_temperature = temperature};
static const h2_pal_pwm_switch_vtable_t pwm = {.set_duty = set_duty,
                                               .get_duty = get_duty};
void h2_app_test_periph_init(h2_app_test_periph_t *p) {
  if (!p)
    return;
  memset(p, 0, sizeof(*p));
  p->api = (h2_pal_periph_api_t){p, &vtable};
  p->button = (h2_pal_button_api_t){p, &buttons};
  p->input = (h2_pal_input_api_t){p, &inputs};
  p->pwm = (h2_pal_pwm_switch_api_t){p, &pwm};
}
int h2_app_test_periph_add(h2_app_test_periph_t *p,
                           const h2_pal_periph_info_t *i,
                           h2_app_test_periph_entry_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!p || !i || !memchr(i->name, 0, sizeof(i->name)) ||
      i->type == H2_PAL_PERIPH_TYPE_ANY ||
      !h2_pal_periph_type_is_valid_filter(i->type) ||
      (i->payload_size && !i->payload))
    return H2_PAL_ERR_INVALID_ARG;
  if (find(p, i->id))
    return H2_PAL_ERR_INVALID_STATE;
  if (p->count >= H2_APP_TEST_PERIPH_MAX)
    return H2_PAL_ERR_NO_SPACE;
  *out = &p->entries[p->count++];
  memset(*out, 0, sizeof(**out));
  (*out)->info = *i;
  return 0;
}
