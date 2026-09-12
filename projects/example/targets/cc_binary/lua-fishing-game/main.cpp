#include "h2_desktop_app_support.h"
#include "h2_lua_fishing_game.h"
#include "layout_config.h"

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

namespace {

constexpr h2_runtime_component_id_t kBackComponentId =
    H2_LUA_FISHING_GAME_COMPONENT_BACK;
constexpr h2_runtime_component_mapping_entry_t kButtons[] = {{kBackComponentId,1u}};

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
    for (size_t i=0;i<1u;++i) {
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
  for (size_t i=0;i<1u;++i) {
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
  int frame_limit = 1;
  int frame_index = 0;
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
  std::string path(tap->path);
  if (tap->frame_limit > 1) {
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "%05d.ppm", tap->frame_index);
    path += suffix;
  }
  FILE *file = std::fopen(path.c_str(), "wbx");
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
  tap->context->captured = ok && ++tap->frame_index >= tap->frame_limit;
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
  const char *no_cache="0", *scroll="";
  const char *weather="auto", *hour="9", *profile="desktop", *scene="idle", *time_ms="", *capture=nullptr, *record=nullptr, *record_frames="360";
  const char *rod="1", *reel="1", *lure="1", *detail="0", *fish_kg="2", *power="ML", *action="F", *brand="1", *check="0";
  for (int i=1;i<argc;i++) {
    if (!std::strcmp(argv[i],"--no-cache")) no_cache="1";
    else if (!std::strncmp(argv[i],"--scroll=",9)) scroll=argv[i]+9;
    else if (!std::strncmp(argv[i],"--profile=",10)) profile=argv[i]+10;
    else if (!std::strncmp(argv[i],"--weather=",10)) weather=argv[i]+10;
    else if (!std::strncmp(argv[i],"--hour=",7)) hour=argv[i]+7;
    else if (!std::strncmp(argv[i],"--scene=",8)) scene=argv[i]+8;
    else if (!std::strncmp(argv[i],"--time-ms=",10)) time_ms=argv[i]+10;
    else if (!std::strncmp(argv[i],"--capture=",10)) capture=argv[i]+10;
    else if (!std::strncmp(argv[i],"--record-prefix=",16)) record=argv[i]+16;
    else if (!std::strncmp(argv[i],"--record-frames=",16)) record_frames=argv[i]+16;
    else if (!std::strncmp(argv[i],"--rod=",6)) rod=argv[i]+6;
    else if (!std::strncmp(argv[i],"--reel=",7)) reel=argv[i]+7;
    else if (!std::strncmp(argv[i],"--lure=",7)) lure=argv[i]+7;
    else if (!std::strncmp(argv[i],"--detail=",9)) detail=argv[i]+9;
    else if (!std::strncmp(argv[i],"--fish-kg=",10)) fish_kg=argv[i]+10;
    else if (!std::strcmp(argv[i],"--check")) check="1";
    else {
      std::fprintf(stderr,"Usage: %s [--scene=demo|physics-demo|perf-demo|idle|overhead|pendulum|iso|fly-back|fly-send|fight|fight-study-1..10|fight-revision-1..12|deck-demo|deck-record|rods|reels|lures|cq|reel-view|lure-view|fish-atlas|fish-atlas-2|fish-atlas-3|bags|fish-bag|fish-bag-preview|retrieve-demo|fight-demo|weather-demo|cast-demo|cast-record] [--profile=desktop|amoled] [--weather=auto|sunny|cloudy|rain] [--hour=0..23] [--time-ms=N] [--capture=new.ppm] [--record-prefix=new/path/frame- --record-frames=360] [--rod=1..11] [--reel=1..19] [--lure=1..18] [--detail=0..1] [--fish-kg=0..50] [--check]\n",argv[0]);
      return 2;
    }
  }
  const auto valid_number=[](const char *value, double lo, double hi) {
    char *end=nullptr; double n=std::strtod(value,&end);
    return end!=value && *end=='\0' && std::isfinite(n) && n>=lo && n<=hi && n==std::floor(n);
  };
  char *fish_end=nullptr;
  double fish_mass=std::strtod(fish_kg,&fish_end);
  bool fish_ok=fish_end!=fish_kg && *fish_end=='\0' && std::isfinite(fish_mass) && fish_mass>=0 && fish_mass<=50;
  bool scene_ok=false,power_ok=false,action_ok=false;
  for (auto v:{"demo","physics-demo","perf-demo","idle","overhead","pendulum","iso","fly-back","fly-send","fight","rods","reels","lures","cq","reel-view","lure-view","fight-study-1","fight-study-2","fight-study-3","fight-study-4","fight-study-5","fight-study-6","fight-study-7","fight-study-8","fight-study-9","fight-study-10","fight-revision-1","fight-revision-2","fight-revision-3","fight-revision-4","fight-revision-5","fight-revision-6","fight-revision-7","fight-revision-8","fight-revision-9","fight-revision-10","fight-revision-11","fight-revision-12","fish-atlas","fish-atlas-2","fish-atlas-3","bags","fish-bag","fish-bag-preview","retrieve-demo","fight-demo","weather-demo","deck-demo","deck-record","cast-demo","cast-record"}) if (!std::strcmp(v,scene)) scene_ok=true;
  for (auto v:{"UL","L","ML","M","MH","H","XH","XXH","XXXH"}) if (!std::strcmp(v,power)) power_ok=true;
  for (auto v:{"R","RF","F","XF"}) if (!std::strcmp(v,action)) action_ok=true;
  if ((record && (capture || !*record || (std::strcmp(scene,"cast-record") && std::strcmp(scene,"deck-record")) || *time_ms)) ||
      !valid_number(record_frames,2,1800) || ((!std::strcmp(scene,"cast-record") || !std::strcmp(scene,"deck-record")) && !record) ||
      (std::strcmp(profile,"desktop") && std::strcmp(profile,"amoled")) || !valid_number(hour,0,23) || (std::strcmp(weather,"auto") && std::strcmp(weather,"sunny") && std::strcmp(weather,"cloudy") && std::strcmp(weather,"rain")) || !fish_ok || !scene_ok || !power_ok || !action_ok || !valid_number(rod,1,11) || !valid_number(reel,1,19) || !valid_number(lure,1,18) || !valid_number(detail,0,1) || !valid_number(brand,1,3) ||
      (*scroll && !valid_number(scroll,0,1000000)) ||
      (*time_ms && !valid_number(time_ms,0,3600000)) || (capture && (!*capture || !*time_ms))) {
    std::fprintf(stderr,"Invalid option; capture requires --time-ms; recording requires cast-record/deck-record and an unused prefix.\n"); return 2;
  }
  h2::desktop::OwnedDisplay display;
  if (h2::desktop::configure_layout(kLayout)!=H2_PAL_OK ||
      h2::desktop::open_display(kLayout,&display)!=H2_DISPLAY_OK ||
      h2_pal_display_open(display.display())!=H2_DISPLAY_OK) return 1;
  AppContext context={&display};
  CaptureDisplay tap={display.display(),capture?capture:record,&context,record?std::atoi(record_frames):1};
  const h2_pal_display_t capture_display={&tap,&kCaptureVtable};
  auto runtime_config=h2::desktop::runtime_config(nullptr);
  h2::desktop::OwnedAudio audio;
  if (!capture && !record && !*time_ms && std::strcmp(check,"1") && h2::desktop::open_audio(false,&audio)==H2_PAL_OK) {
    runtime_config.audio=audio.api();
  }
  runtime_config.display=(capture||record)?&capture_display:display.display();
  runtime_config.touch=display.touch();
  runtime_config.component_mapper=&kMapper;
  h2_runtime_t *runtime=nullptr;
  auto result=h2_runtime_init(&runtime_config,&runtime);
  if (result!=H2_PAL_OK) return 1;
  result=h2_runtime_input_start(runtime,nullptr);
  if (result==H2_PAL_OK) {
    const h2_lua_fishing_game_config_t config={
      .profile=profile,.weather=weather,.hour=hour,.scene=scene,.time_ms=time_ms,.rod=rod,.reel=reel,.lure=lure,.detail=detail,.fish_kg=fish_kg,.power=power,.action=action,.brand=brand,.check=check,
      .no_cache=no_cache,.scroll=scroll,
      .back_component_id=kBackComponentId,.should_stop=should_stop,.should_stop_user=&context,
      .on_ready=nullptr,.on_ready_user=nullptr,
    };
    std::printf("AMOLED Fishing Game: click sea to cast; swipe left for gear; swipe right to return. No textures.\n");
    result=h2_lua_fishing_game_run(runtime,&config);
  }
  (void)h2::desktop::poll_events(&display);
  h2_runtime_deinit(runtime);
  return result==H2_PAL_OK?0:1;
}
