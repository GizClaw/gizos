#include "h2_g99_loader_state.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PRODUCT "GizPocket"

typedef struct phase_info {
  h2_loader_install_phase_t phase;
  const char* name;
  uint32_t start;
  uint32_t span;
} phase_info_t;

/* Writing the image dominates an install, so it owns most of the bar. */
static const phase_info_t kPhases[] = {
    {H2_LOADER_INSTALL_PHASE_VALIDATE, "Validating", 0u, 5u},
    {H2_LOADER_INSTALL_PHASE_IMAGE, "Writing image", 5u, 75u},
    {H2_LOADER_INSTALL_PHASE_DATA, "Installing data", 80u, 5u},
    {H2_LOADER_INSTALL_PHASE_PIXA, "Expanding PIXA", 85u, 5u},
    {H2_LOADER_INSTALL_PHASE_VERIFY, "Verifying", 90u, 9u},
    {H2_LOADER_INSTALL_PHASE_DONE, "Installed", 100u, 0u},
};

static const phase_info_t* find(h2_loader_install_phase_t phase) {
  for (size_t i = 0u; i < sizeof(kPhases) / sizeof(kPhases[0]); ++i)
    if (kPhases[i].phase == phase) return &kPhases[i];
  return NULL;
}

/* completed / total in percent, safe for any 64-bit values. */
static uint32_t ratio_percent(uint64_t completed, uint64_t total) {
  if (total == 0u) return 100u;
  if (completed >= total) return 100u;
  if (completed <= UINT64_MAX / 100u)
    return (uint32_t)(completed * 100u / total);
  return (uint32_t)(completed / (total / 100u));
}

uint32_t h2_g99_loader_phase_percent(h2_loader_install_phase_t phase,
                                     uint64_t completed, uint64_t total) {
  const phase_info_t* info = find(phase);
  if (info == NULL) return 0u;
  uint32_t ratio = ratio_percent(completed, total);
  if (ratio > 100u) ratio = 100u;
  return info->start + info->span * ratio / 100u;
}

const char* h2_g99_loader_phase_name(h2_loader_install_phase_t phase) {
  const phase_info_t* info = find(phase);
  return info != NULL ? info->name : "Installing";
}

void h2_g99_loader_state_init(h2_g99_loader_state_t* state) {
  if (state == NULL) return;
  memset(state, 0, sizeof(*state));
  state->screen = H2_G99_LOADER_SCREEN_BRAND;
}

void h2_g99_loader_state_ready(h2_g99_loader_state_t* state) {
  if (state == NULL) return;
  state->screen = H2_G99_LOADER_SCREEN_READY;
  state->launching = false;
}

void h2_g99_loader_state_installing(h2_g99_loader_state_t* state) {
  if (state == NULL) return;
  state->screen = H2_G99_LOADER_SCREEN_INSTALLING;
  state->installed = true;
  state->launching = false;
  state->phase = 0;
  state->percent = 0u;
}

void h2_g99_loader_state_progress(h2_g99_loader_state_t* state,
                                  h2_loader_install_phase_t phase,
                                  uint64_t completed, uint64_t total) {
  if (state == NULL) return;
  const uint32_t percent = h2_g99_loader_phase_percent(phase, completed, total);
  state->screen = H2_G99_LOADER_SCREEN_PROGRESS;
  state->installed = true;
  state->launching = false;
  state->phase = phase;
  if (percent > state->percent) state->percent = percent;
}

void h2_g99_loader_state_failed(h2_g99_loader_state_t* state, int code) {
  if (state == NULL) return;
  state->screen = H2_G99_LOADER_SCREEN_FAILED;
  state->code = code;
}

void h2_g99_loader_state_launching(h2_g99_loader_state_t* state) {
  if (state == NULL) return;
  state->launching = true;
  state->screen = state->installed ? H2_G99_LOADER_SCREEN_STARTING
                                   : H2_G99_LOADER_SCREEN_BRAND;
}

void h2_g99_loader_state_view(const h2_g99_loader_state_t* state,
                              h2_g99_loader_view_t* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (state == NULL) return;
  out->status_visible = true;
  switch (state->screen) {
    case H2_G99_LOADER_SCREEN_BRAND:
      out->status_visible = false;
      return;
    case H2_G99_LOADER_SCREEN_READY:
      (void)snprintf(out->status, sizeof(out->status), "Ready to transfer");
      return;
    case H2_G99_LOADER_SCREEN_INSTALLING:
      (void)snprintf(out->status, sizeof(out->status),
                     "Installing " PRODUCT "...");
      return;
    case H2_G99_LOADER_SCREEN_PROGRESS:
      (void)snprintf(out->status, sizeof(out->status), "%s... %u%%",
                     h2_g99_loader_phase_name(state->phase),
                     (unsigned)state->percent);
      out->bar_visible = true;
      out->bar_percent = state->percent;
      return;
    case H2_G99_LOADER_SCREEN_FAILED:
      /* Keep the last progress on the bar, in the failure tone. */
      (void)snprintf(out->status, sizeof(out->status), "%s failed (%d)",
                     state->launching ? "Starting " PRODUCT
                                      : h2_g99_loader_phase_name(state->phase),
                     state->code);
      out->error = true;
      out->bar_visible = state->phase != 0;
      out->bar_percent = state->percent;
      return;
    case H2_G99_LOADER_SCREEN_STARTING:
      (void)snprintf(out->status, sizeof(out->status),
                     "Starting " PRODUCT "...");
      return;
  }
}
