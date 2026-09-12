#ifndef H2_FISHING_H106_PREVIEW_H
#define H2_FISHING_H106_PREVIEW_H
#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/hal/h2_pal_touch.h"
#include <algorithm>

// Direct 240x240 display PAL: no framebuffer, filter or blit adapter.
// Only desktop pointer input is mapped to shared authored layout units.
class H106Preview {
 public:
  static constexpr int width=240,height=240,physical=240,authored=448;
  explicit H106Preview(const h2_pal_display_t *display,const h2_pal_touch_api_t *touch)
      : target_(display),touch_target_(touch) {}
  const h2_pal_display_t *display() const {return target_;}
  const h2_pal_touch_api_t *touch() const {return &touch_;}
  static int logical(int pixel,int extent) {
    pixel=std::clamp(pixel,0,physical-1);
    return (2*pixel+1)*extent/(2*physical);
  }
 private:
  const h2_pal_display_t *target_;
  const h2_pal_touch_api_t *touch_target_;
  static H106Preview &self(void *u) {return *static_cast<H106Preview *>(u);}
  static h2_pal_result_t open(void *u) {return h2_pal_touch_open(self(u).touch_target_);}
  static h2_pal_result_t info(void *u,h2_pal_touch_info_t *out) {
    h2_pal_touch_info_t actual{};auto rc=h2_pal_touch_get_info(self(u).touch_target_,&actual);
    if(rc!=H2_PAL_OK)return rc;
    if(actual.width!=physical || actual.height!=physical)return H2_PAL_ERR_INVALID_ARG;
    *out={authored,authored};return H2_PAL_OK;
  }
  static h2_pal_result_t poll(void *u,h2_pal_touch_event_t *out) {
    auto rc=h2_pal_touch_poll_event(self(u).touch_target_,out);
    if(rc==H2_PAL_OK){out->x=logical(out->x,authored);out->y=logical(out->y,authored);}
    return rc;
  }
  static h2_pal_result_t close(void *u) {return h2_pal_touch_close(self(u).touch_target_);}
  inline static const h2_pal_touch_vtable_t vtable_={open,info,poll,close};
  const h2_pal_touch_api_t touch_={this,&vtable_};
};
#endif
