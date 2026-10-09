#ifndef MOSAICO_DIAGNOSTICS_H
#define MOSAICO_DIAGNOSTICS_H
#include "h2_runtime.h"
typedef struct {
    char battery[40], audio[40], wifi[40], ble[40], stage[40], camera[40], hotplug[40];
    unsigned audio_passed, audio_failed, audio_blocked;
    unsigned wifi_aps, ble_reports;
} mosaico_diagnostics_t;
void mosaico_camera_probe(void);
void mosaico_camera_e2e_init(void);
bool mosaico_camera_e2e_poll(h2_runtime_t *runtime);
bool mosaico_camera_view_active(void);
int mosaico_camera_view_toggle(void);
int mosaico_camera_view_tick(h2_runtime_t *runtime);
void mosaico_audible_test(h2_runtime_t *runtime);
extern mosaico_diagnostics_t mosaico_diagnostics;
void mosaico_run_diagnostics(h2_runtime_t *runtime, void (*refresh)(void *), void *user);
#endif
