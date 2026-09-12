#include "h106_preview.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(expr) do {if(!(expr)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expr);std::abort();}} while(0)
static h2_pal_result_t info(void *,h2_pal_touch_info_t *out) {*out={240,240};return H2_PAL_OK;}
static h2_pal_result_t poll(void *,h2_pal_touch_event_t *out) {*out={H2_PAL_TOUCH_EVENT_MOVE,239,0};return H2_PAL_OK;}
int main() {
  const h2_pal_display_t display={nullptr,nullptr};
  const h2_pal_touch_vtable_t vtable={nullptr,info,poll,nullptr};
  const h2_pal_touch_api_t touch={nullptr,&vtable};
  H106Preview p(&display,&touch);
  CHECK(p.display()==&display);
  CHECK(H106Preview::width==240 && H106Preview::height==240);
  CHECK(sizeof(p)<128);
  h2_pal_touch_info_t ti{};CHECK(h2_pal_touch_get_info(p.touch(),&ti)==H2_PAL_OK);
  CHECK(ti.width==448 && ti.height==448);
  h2_pal_touch_event_t e{};CHECK(h2_pal_touch_poll_event(p.touch(),&e)==H2_PAL_OK);
  CHECK(e.x==447 && e.y==0);
  CHECK(H106Preview::logical(-1,448)==0 && H106Preview::logical(240,448)==447);
  std::puts("PASS native 240x240 display passthrough; authored pointer mapping only");
}
