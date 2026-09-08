#include "h2_desktop_app_support.h"
#include "h2_lua_qi_duel.h"
#include "layout_config.h"

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

namespace {

constexpr h2_runtime_component_id_t kBackComponentId =
    H2_LUA_QI_DUEL_COMPONENT_BACK;
constexpr bool kH106 = h2_desktop_layout::width==240 && h2_desktop_layout::height==240;
constexpr h2_runtime_component_mapping_entry_t kButtons[] = {
    {kBackComponentId,1u},
    {H2_LUA_QI_DUEL_COMPONENT_VOLUME_UP,106u},
    {H2_LUA_QI_DUEL_COMPONENT_VOLUME_DOWN,107u},
    {H2_LUA_QI_DUEL_COMPONENT_RECORD,108u},
};

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,        h2_desktop_layout::title,
    h2_desktop_layout::width,           h2_desktop_layout::height,
    h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

h2_pal_result_t list_components(void *, h2_runtime_component_t filter,
                                h2_runtime_component_mapping_cb_t callback,
                                void *callback_user) {
  if (callback == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (filter == H2_RUNTIME_COMPONENT_BUTTON) {
    for (size_t i=0;i<(kH106?4u:1u);++i) {
      h2_pal_result_t result=callback(callback_user,&kButtons[i]);
      if (result!=H2_PAL_OK) return result;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t get_periph_id(void *, h2_runtime_component_id_t component_id,
                              h2_pal_periph_id_t *out_periph_id) {
  if (out_periph_id == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t i=0;i<(kH106?4u:1u);++i) {
    if (component_id==kButtons[i].component_id) {
      *out_periph_id=kButtons[i].periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapper_vtable_t kMapperVtable = {
    list_components,
    get_periph_id,
};
const h2_runtime_component_mapper_t kMapper = {nullptr, &kMapperVtable};

struct AppContext {
  h2::desktop::OwnedDisplay *display;
  std::atomic_bool captured{false};
};

// A capture tap forwards to the actual SDL PAL. It records the very pixels
// submitted by the Lua renderer, not a separately reimplemented test renderer.
struct CaptureDisplay {
  const h2_pal_display_t *target;
  const char *path;
  AppContext *context;
  std::vector<uint16_t> pixels = std::vector<uint16_t>(h2_desktop_layout::width * h2_desktop_layout::height);
  bool has_frame = false;
};

int capture_open(void *user) {
  return h2_pal_display_open(static_cast<CaptureDisplay *>(user)->target);
}
int capture_info(void *user, h2_display_info_t *info) {
  return h2_pal_display_get_info(static_cast<CaptureDisplay *>(user)->target, info);
}
int capture_draw(void *user, const h2_display_rect_t *rect, const void *pixels,
                 size_t stride, h2_display_pixel_format_t format) {
  auto *tap = static_cast<CaptureDisplay *>(user);
  if (format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 || rect->x + rect->width > h2_desktop_layout::width ||
      rect->y + rect->height > h2_desktop_layout::height || stride < size_t(rect->width) * 2u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  int result = h2_pal_display_draw_bitmap(tap->target, rect, pixels, stride, format);
  if (result != H2_DISPLAY_OK) return result;
  for (int row = 0; row < rect->height; ++row) {
    std::memcpy(tap->pixels.data() + (rect->y + row) * h2_desktop_layout::width + rect->x,
                static_cast<const uint8_t *>(pixels) + row * stride,
                size_t(rect->width) * 2u);
  }
  tap->has_frame = true;
  return result;
}
int capture_present(void *user) {
  auto *tap = static_cast<CaptureDisplay *>(user);
  int result = h2_pal_display_present(tap->target);
  if (result != H2_DISPLAY_OK || !tap->has_frame || tap->context->captured)
    return result;
  // Exclusive creation prevents accidentally overwriting a user's file.
  FILE *file = std::fopen(tap->path, "wbx");
  if (!file) {
    std::perror("capture");
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  bool ok = std::fprintf(file, "P6\n%d %d\n255\n",int(h2_desktop_layout::width),int(h2_desktop_layout::height)) > 0;
  for (uint16_t color : tap->pixels) {
    unsigned r = color >> 11u, g = (color >> 5u) & 63u, b = color & 31u;
    uint8_t rgb[] = {uint8_t((r << 3u) | (r >> 2u)),
                     uint8_t((g << 2u) | (g >> 4u)),
                     uint8_t((b << 3u) | (b >> 2u))};
    if (std::fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) ok = false;
  }
  if (std::fclose(file) != 0) ok = false;
  tap->context->captured = ok;
  return ok ? H2_DISPLAY_OK : H2_DISPLAY_ERR_INVALID_ARG;
}
int capture_brightness(void *user, uint32_t percent) {
  return h2_pal_display_set_brightness_percent(static_cast<CaptureDisplay *>(user)->target, percent);
}
int capture_close(void *user) {
  return h2_pal_display_close(static_cast<CaptureDisplay *>(user)->target);
}
const h2_pal_display_vtable_t kCaptureVtable = {
    capture_open, capture_info, capture_draw, capture_present,
    capture_brightness, capture_close,
};

int should_stop(void *user) {
  auto *context = static_cast<AppContext *>(user);
  return context == nullptr || context->display == nullptr ||
         context->captured ||
         h2::desktop::poll_events(context->display) != 0;
}

} // namespace

int main(int argc, char **argv) {
  const char *layer = "full", *time_ms = nullptr, *capture = nullptr;
  const char *health_fx = nullptr;
  const char *charge_fx = nullptr;
  const char *qi = nullptr;
  const char *selected = nullptr, *drag = nullptr;
  const char *action = nullptr, *actor = nullptr;
  const char *impact = nullptr;
  bool rehearsal=false, game_probe=false;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--layer=", 8) == 0) layer = argv[i] + 8;
    else if (std::strncmp(argv[i], "--time-ms=", 10) == 0) time_ms = argv[i] + 10;
    else if (std::strncmp(argv[i], "--capture=", 10) == 0) capture = argv[i] + 10;
    else if (std::strncmp(argv[i], "--health-fx=", 12) == 0) health_fx = argv[i] + 12;
    else if (std::strncmp(argv[i], "--charge-fx=", 12) == 0) charge_fx = argv[i] + 12;
    else if (std::strncmp(argv[i], "--qi=", 5) == 0) qi = argv[i] + 5;
    else if (std::strncmp(argv[i], "--selected=", 11) == 0) selected = argv[i] + 11;
    else if (std::strncmp(argv[i], "--drag=", 7) == 0) drag = argv[i] + 7;
    else if (std::strncmp(argv[i], "--action=", 9) == 0) action = argv[i] + 9;
    else if (std::strncmp(argv[i], "--actor=", 8) == 0) actor = argv[i] + 8;
    else if (std::strncmp(argv[i], "--impact=", 9) == 0) impact = argv[i] + 9;
    else if (std::strcmp(argv[i], "--rehearsal") == 0) rehearsal=true;
    else if (std::strcmp(argv[i], "--game") == 0) game_probe=true;
    else {
      std::fprintf(stderr, "Default: native game mode menu. Use --rehearsal for visual-only controls, --game to enable the menu with fixed-time probes.\n");
      std::fprintf(stderr, "Usage: %s [--layer=full|walls|wheel|arena|dust|particles|opponent|hand-left|hand-right|arena-dust|scene7|hud|scene8|carousel-frame|charge-cells|charge-base|scene11|carousel|skill-charge|skill-wave|skill-absorb|skill-guard] [--time-ms=N] [--selected=0..3] [--drag=-90..90] [--health-fx=player-down|player-up|enemy-down|enemy-up] [--charge-fx=down|up] [--impact=combo|armor-break] [--capture=new.ppm]\n", argv[0]);
      return 2;
    }
  }
  char *end = nullptr;
  double fixed = time_ms ? std::strtod(time_ms, &end) : 0;
  const auto valid_number = [](const char *value, double low, double high, bool integer) {
    if (!value) return true;
    char *tail = nullptr;
    double parsed = std::strtod(value, &tail);
    return tail != value && *tail == '\0' && std::isfinite(parsed) &&
           parsed >= low && parsed <= high && (!integer || parsed == std::floor(parsed));
  };
  bool valid_layer = false;
  for (const char *candidate : {"full","walls","wheel","arena","dust","particles",
                                "opponent","hand-left","hand-right","arena-dust","scene7","hud","scene8",
                                "carousel-frame","charge-cells","charge-base","scene11","carousel",
                                "skill-charge","skill-wave","skill-absorb","skill-guard"})
    if (std::strcmp(layer, candidate) == 0) valid_layer = true;
  bool valid_fx = health_fx == nullptr;
  bool valid_action = action == nullptr, valid_actor = actor == nullptr;
  bool valid_impact = impact == nullptr || std::strcmp(impact,"combo")==0 ||
                      std::strcmp(impact,"armor-break")==0;
  for (const char *candidate : {"charge","wave","absorb","guard"})
    if (action && std::strcmp(action,candidate)==0) valid_action=true;
  for (const char *candidate : {"both","player","opponent"})
    if (actor && std::strcmp(actor,candidate)==0) valid_actor=true;
  for (const char *candidate : {"player-down","player-up","enemy-down","enemy-up"})
    if (health_fx && std::strcmp(health_fx,candidate)==0) valid_fx=true;
  if (!valid_layer || !valid_fx || !valid_action || !valid_actor || !valid_impact || !valid_number(qi,0,5,true) || !valid_number(selected,0,3,true) || !valid_number(drag,-90,90,false) ||
      (charge_fx && std::strcmp(charge_fx,"up")!=0 && std::strcmp(charge_fx,"down")!=0) ||
      (time_ms && (end == time_ms || *end != '\0' || !std::isfinite(fixed) || fixed < 0)) ||
      (capture && (capture[0] == '\0' || time_ms == nullptr))) {
    std::fprintf(stderr, "Invalid layer/time; capture requires --time-ms.\n");
    return 2;
  }
  h2::desktop::OwnedDisplay display;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display) != H2_DISPLAY_OK ||
      h2_pal_display_open(display.display()) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: display initialization failed\n",
                 kLayout.app_name);
    return 1;
  }

  h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
  AppContext context = {&display};
  CaptureDisplay tap = {display.display(), capture, &context};
  const h2_pal_display_t capture_display = {&tap, &kCaptureVtable};
  runtime_config.display = capture ? &capture_display : display.display();
  runtime_config.touch = display.touch();
  runtime_config.component_mapper = &kMapper;

  h2_runtime_t *runtime = nullptr;
  h2_pal_result_t result = h2_runtime_init(&runtime_config, &runtime);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: Runtime init failed (%d)\n",
                 kLayout.app_name, result);
    return 1;
  }
  result = h2_runtime_input_start(runtime, nullptr);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: Runtime input start failed (%d)\n",
                 kLayout.app_name, result);
    h2_runtime_deinit(runtime);
    return 1;
  }

  const h2_lua_qi_duel_config_t config = {
      .back_component_id = kBackComponentId,
      .should_stop = should_stop,
      .should_stop_user = &context,
      .on_ready = nullptr,
      .on_ready_user = nullptr,
      .layer = layer,
      .time_ms = time_ms,
      .health_fx = health_fx,
      .charge_fx = charge_fx,
      .qi = qi,
      .click_controls = 1,
      .battle = game_probe || (!rehearsal && !time_ms && !action && std::strcmp(layer,"full")==0),
      .selected = selected,
      .drag = drag,
      .action = action,
      .actor = actor,
      .impact = impact,
  };
  result = h2_lua_qi_duel_run(runtime, &config);
  (void)h2::desktop::poll_events(&display);
  h2_runtime_deinit(runtime);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: App failed (%d)\n", kLayout.app_name,
                 result);
    return 1;
  }
  return 0;
}
