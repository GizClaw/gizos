#include "h2_lua_qi_duel.h"

#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"
#include "h2_runtime_test.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  size_t display_open;
  size_t draw;
  size_t present;
  size_t display_close;
  size_t touch_open;
  size_t touch_close;
  size_t touch_event_sent;
  size_t ready;
  int inspector_layer; /* one-based index in the inspector test table */
  uint32_t frame_hash;
  uint16_t pixels[368u*448u];
  int tap_x, tap_y;
  int touch_synced;
  size_t tap_count;
  int drag_distance;
  int drag_dy;
  int skip_move;
  int h106;
  int no_touch;
  h2_runtime_test_control_t *button_control;
  unsigned button_id;
  int button_sent;
  int button_burst;
  int game_lock_probe;
  int intro_probe;
  int result_key_probe;
} fixture_t;

static h2_pal_result_t button_map_list(void *user,h2_runtime_component_t kind,
    h2_runtime_component_mapping_cb_t callback,void *context) {
  (void)user;
  if(kind==H2_RUNTIME_COMPONENT_BUTTON)for(unsigned id=9;id<=11;id++) {
    const h2_runtime_component_mapping_entry_t entry={id,id+97};
    h2_pal_result_t result=callback(context,&entry);
    if(result!=H2_PAL_OK)return result;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t button_map_get(void *user,h2_runtime_component_id_t id,h2_pal_periph_id_t *out) {
  (void)user;
  if(id<9 || id>11)return H2_PAL_ERR_NOT_FOUND;
  *out=id+97;return H2_PAL_OK;
}
static h2_pal_result_t button_periph_get(void *user,h2_pal_periph_id_t id,h2_pal_periph_info_t *out) {
  (void)user;
  if(id<106 || id>108)return H2_PAL_ERR_NOT_FOUND;
  *out=(h2_pal_periph_info_t){.id=id,.type=H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,.name="test-key"};
  return H2_PAL_OK;
}
static h2_pal_result_t button_periph_list(void *user,h2_pal_periph_type_t kind,h2_pal_periph_cb_t callback,void *context) {
  if(kind==H2_PAL_PERIPH_TYPE_ANY || kind==H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    for(unsigned id=106;id<=108;id++) {
      h2_pal_periph_info_t info;
      button_periph_get(user,id,&info);
      h2_pal_result_t result=callback(context,&info);
      if(result!=H2_PAL_OK)return result;
    }
  return H2_PAL_OK;
}

static int display_open(void *user) {
  ((fixture_t *)user)->display_open++;
  return H2_PAL_OK;
}

static int display_info(void *user, h2_display_info_t *out_info) {
  fixture_t *fixture=user;
  *out_info = (h2_display_info_t){fixture->h106?240:368, fixture->h106?240:448, H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}

static int draw_bitmap(void *user, const h2_display_rect_t *rect,
                       const void *pixels, size_t stride,
                       h2_display_pixel_format_t format) {
  fixture_t *fixture = user;
  size_t width=fixture->h106?240u:368u,height=fixture->h106?240u:448u;
  assert(rect->x>=0 && rect->y>=0 && rect->width>0 && rect->height>0 &&
         rect->x+rect->width<=(int)width && rect->y+rect->height<=(int)height);
  assert(pixels != NULL && stride == width * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  for(int row=0;row<rect->height;row++)
    memcpy(fixture->pixels+(size_t)(rect->y+row)*width+rect->x,
           (const uint8_t *)pixels+(size_t)row*stride,(size_t)rect->width*2);
  if (fixture->inspector_layer) {
    const uint16_t *frame = fixture->pixels;
    uint32_t hash = 2166136261u;
    size_t lit = 0u;
    for (size_t i = 0u; i < width * height; ++i) {
      hash = (hash ^ frame[i]) * 16777619u;
      if (frame[i] != 0u) lit++;
      if(fixture->h106) {
        if(fixture->inspector_layer==11 && i>=width*41u)assert(frame[i]==0);
        if(fixture->inspector_layer==6 && (i<width*60u || i>=width*149u))assert(frame[i]==0);
        if(fixture->inspector_layer>=18 && fixture->inspector_layer<=21 && i<width*160u)assert(frame[i]==0);
        continue;
      }
      if (fixture->inspector_layer == 1 && i >= 368u * 250u)
        assert(frame[i] == 0u);
      if (fixture->inspector_layer == 2 &&
          (i < 368u * 225u || i >= 368u * 325u)) assert(frame[i] == 0u);
      if (fixture->inspector_layer == 3 && i >= 368u * 325u)
        assert(frame[i] == 0u);
      if (fixture->inspector_layer == 4 &&
          (i < 368u * 220u || i >= 368u * 310u)) assert(frame[i] == 0u);
      if (fixture->inspector_layer == 6 &&
          (i < 368u * 100u || i >= 368u * 270u)) assert(frame[i] == 0u);
      if ((fixture->inspector_layer == 7 || fixture->inspector_layer == 8) &&
          (i < 368u * 160u || i >= 368u * 365u)) assert(frame[i] == 0u);
      if (fixture->inspector_layer == 11 && i >= 368u*120u)
        assert(frame[i] == 0u);
      if (fixture->inspector_layer == 13 && (i < 368u*267u || i >= 368u*420u))
        assert(frame[i] == 0u);
      if (fixture->inspector_layer == 14 && (i < 368u*280u || i >= 368u*415u))
        assert(frame[i] == 0u);
      if (fixture->inspector_layer >= 18 && fixture->inspector_layer <= 21 && i < 368u*320u)
        assert(frame[i] == 0u);
    }
    if(!fixture->result_key_probe)assert(lit > 50u);
    if (fixture->draw != 0u && !fixture->tap_count && !fixture->drag_distance && !fixture->drag_dy && !fixture->button_id && !fixture->game_lock_probe && !fixture->intro_probe) assert(hash == fixture->frame_hash);
    fixture->frame_hash = hash;
  }
  fixture->draw++;
  return H2_PAL_OK;
}

static int display_present(void *user) {
  ((fixture_t *)user)->present++;
  return H2_PAL_OK;
}

static int display_close(void *user) {
  ((fixture_t *)user)->display_close++;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_open(void *user) {
  fixture_t *fixture = user;
  fixture->touch_open++;
  return fixture->no_touch ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
}

static h2_pal_result_t touch_info(void *user, h2_pal_touch_info_t *out_info) {
  fixture_t *fixture=user;
  *out_info = (h2_pal_touch_info_t){fixture->h106?240u:368u, fixture->h106?240u:448u};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll(void *user, h2_pal_touch_event_t *out_event) {
  fixture_t *fixture = user;
  if(fixture->game_lock_probe) {
    if(!fixture->touch_synced){fixture->touch_synced=1;return H2_PAL_ERR_WOULD_BLOCK;}
    unsigned event=(unsigned)fixture->touch_event_sent;
    unsigned limit=fixture->game_lock_probe==1?4u:6u;
    if(event>=limit)return H2_PAL_ERR_WOULD_BLOCK;
    int x=event<4?120:(fixture->game_lock_probe==2?50:190);
    *out_event=(h2_pal_touch_event_t){event%2?H2_PAL_TOUCH_EVENT_UP:H2_PAL_TOUCH_EVENT_DOWN,
        x,event<2?120:215};
    fixture->touch_event_sent++;
    return H2_PAL_OK;
  }
  if(fixture->tap_x && !fixture->touch_synced) {
    fixture->touch_synced=1;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if(fixture->tap_count) {
    if(fixture->touch_event_sent>=fixture->tap_count*2)return H2_PAL_ERR_WOULD_BLOCK;
    *out_event=(h2_pal_touch_event_t){fixture->touch_event_sent%2?H2_PAL_TOUCH_EVENT_UP:H2_PAL_TOUCH_EVENT_DOWN,
        fixture->tap_x,fixture->tap_y};
    fixture->touch_event_sent++;
    return H2_PAL_OK;
  }
  if(fixture->drag_distance || fixture->drag_dy) {
    if(fixture->touch_event_sent>=(fixture->skip_move?2u:3u))return H2_PAL_ERR_WOULD_BLOCK;
    const h2_pal_touch_event_kind_t kinds[]={H2_PAL_TOUCH_EVENT_DOWN,H2_PAL_TOUCH_EVENT_MOVE,H2_PAL_TOUCH_EVENT_UP};
    *out_event=(h2_pal_touch_event_t){kinds[fixture->touch_event_sent+(fixture->skip_move && fixture->touch_event_sent?1:0)],
      fixture->tap_x+(fixture->touch_event_sent?fixture->drag_distance:0),
      fixture->tap_y+(fixture->touch_event_sent?fixture->drag_dy:0)};
    fixture->touch_event_sent++;
    return H2_PAL_OK;
  }
  if (fixture->touch_event_sent != 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  fixture->touch_event_sent++;
  *out_event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN,
      fixture->tap_x ? fixture->tap_x : 184, fixture->tap_y ? fixture->tap_y : 394};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_close(void *user) {
  ((fixture_t *)user)->touch_close++;
  return H2_PAL_OK;
}

static int should_stop(void *user) {
  fixture_t *fixture = user;
  if(fixture->button_id && fixture->ready && !fixture->button_sent) {
    const h2_runtime_button_down_event_t down={10};
    const h2_runtime_button_up_event_t up={10,20};
    const h2_runtime_button_action_event_t action={.pressed_at_ms=10,.released_at_ms=20};
    /* Duplicate DOWN and the Runtime's separate ACTION must not fire twice. */
    for(int i=0;i<(fixture->button_burst?fixture->button_burst:2);i++)assert(h2_runtime_test_emit_event(fixture->button_control,
      H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN,H2_RUNTIME_COMPONENT_BUTTON,fixture->button_id,10,&down,sizeof(down))==H2_PAL_OK);
    assert(h2_runtime_test_emit_event(fixture->button_control,
      H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP,H2_RUNTIME_COMPONENT_BUTTON,fixture->button_id,20,&up,sizeof(up))==H2_PAL_OK);
    assert(h2_runtime_test_emit_event(fixture->button_control,
      H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION,H2_RUNTIME_COMPONENT_BUTTON,fixture->button_id,20,&action,sizeof(action))==H2_PAL_OK);
    fixture->button_sent=1;
  }
  return fixture->ready != 0u && fixture->present >= 6u+(fixture->intro_probe?4u:0u)+fixture->tap_count*2+(fixture->game_lock_probe?6u:0u);
}

static int never_stop(void *user) {
  (void)user;
  return 0;
}

static h2_pal_result_t ready(void *user) {
  ((fixture_t *)user)->ready++;
  return H2_PAL_OK;
}

static h2_pal_result_t fail_ready(void *user) {
  ((fixture_t *)user)->ready++;
  return H2_PAL_ERR_INVALID_STATE;
}

int main(void) {
  static const h2_pal_display_vtable_t display_vtable = {
      .open = display_open,
      .get_info = display_info,
      .draw_bitmap = draw_bitmap,
      .present = display_present,
      .close = display_close,
  };
  static const h2_pal_touch_vtable_t touch_vtable = {
      .open = touch_open,
      .get_info = touch_info,
      .poll_event = touch_poll,
      .close = touch_close,
  };
  fixture_t fixture = {0};
  const h2_pal_display_api_t display = {&fixture, &display_vtable};
  const h2_pal_touch_api_t touch = {&fixture, &touch_vtable};
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "test", "desktop", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      &display);
  runtime_config.log = h2_desktop_platform_log_api();
  runtime_config.task = h2_desktop_platform_task_api();
  runtime_config.sync = h2_desktop_platform_sync_api();
  runtime_config.touch = &touch;
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&runtime_config, &runtime) == H2_PAL_OK);
  assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
  const h2_lua_qi_duel_config_t valid_config = {
      .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .should_stop = should_stop,
      .should_stop_user = &fixture,
  };
  assert(h2_lua_qi_duel_run(NULL, &valid_config) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_qi_duel_run(runtime, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_qi_duel_run(
             runtime, &(h2_lua_qi_duel_config_t){
                          .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                      }) == H2_PAL_ERR_INVALID_ARG);

  assert(h2_lua_qi_duel_run(
             runtime, &(h2_lua_qi_duel_config_t){
                          .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                          .should_stop = never_stop,
                          .on_ready = fail_ready,
                          .on_ready_user = &fixture,
                      }) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.ready == 1u);
  assert(fixture.display_open == 1u && fixture.display_close == 1u);
  assert(fixture.touch_open == 1u && fixture.touch_close == 1u);

  fixture = (fixture_t){0};
  assert(h2_lua_qi_duel_run(
             runtime, &(h2_lua_qi_duel_config_t){
                          .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                          .should_stop = should_stop,
                          .should_stop_user = &fixture,
                          .on_ready = ready,
                          .on_ready_user = &fixture,
                      }) == H2_PAL_OK);
  assert(fixture.ready == 1u);
  assert(fixture.display_open == 1u && fixture.display_close == 1u);
  assert(fixture.touch_open == 1u && fixture.touch_close == 1u);
  assert(fixture.draw >= 1u && fixture.present >= 6u);
  const char *layers[] = {"walls", "wheel", "arena", "dust", "particles",
      "opponent", "hand-left", "hand-right", "arena-dust", "scene7", "hud", "scene8",
      "carousel-frame","charge-cells","charge-base","scene11","carousel",
      "skill-charge","skill-wave","skill-absorb","skill-guard","full"};
  for (size_t layer = 0u; layer < sizeof(layers)/sizeof(layers[0]); ++layer) {
    fixture = (fixture_t){.inspector_layer = (int)layer + 1};
    assert(h2_lua_qi_duel_run(
               runtime, &(h2_lua_qi_duel_config_t){
                            .should_stop = should_stop,
                            .should_stop_user = &fixture,
                            .on_ready = ready,
                            .on_ready_user = &fixture,
                            .layer = layers[layer], .time_ms = "1875",
                        }) == H2_PAL_OK);
    assert(fixture.draw >= 1u && fixture.ready == 1u);
    assert(fixture.display_open == 1u && fixture.display_close == 1u);
  }
  const char *effects[]={"player-down","player-up","enemy-down","enemy-up"};
  const int taps[][2]={{50,85},{160,85},{210,40},{320,40}};
  uint32_t effect_start_hash[4];
  for(size_t i=0;i<4;i++)for(size_t sample=0;sample<2;sample++) {
    fixture=(fixture_t){.inspector_layer=11};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="hud",.time_ms=sample?"120":"0",.health_fx=effects[i]})==H2_PAL_OK);
    if(!sample)effect_start_hash[i]=fixture.frame_hash;
    else assert(effect_start_hash[i]!=fixture.frame_hash);
    assert(fixture.display_open==1u && fixture.display_close==1u);
  }
  /* Real PAL pointer-down must produce the same first effect frame as a probe. */
  for(size_t i=0;i<4;i++) {
    fixture=(fixture_t){.inspector_layer=11,.tap_x=taps[i][0],.tap_y=taps[i][1]};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="hud",.time_ms="120"})==H2_PAL_OK);
    assert(fixture.frame_hash==effect_start_hash[i]);
  }
  uint32_t charge_start_hash[2];
  for(size_t i=0;i<2;i++)for(size_t sample=0;sample<2;sample++) {
    fixture=(fixture_t){.inspector_layer=14};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="charge-cells",.time_ms=sample?"130":"0",.charge_fx=i?"up":"down"})==H2_PAL_OK);
    if(!sample)charge_start_hash[i]=fixture.frame_hash;
    else assert(charge_start_hash[i]!=fixture.frame_hash);
  }
  for(size_t i=0;i<2;i++) {
    fixture=(fixture_t){.inspector_layer=14,.tap_x=i?250:110,.tap_y=355};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="charge-cells",.time_ms="130"})==H2_PAL_OK);
    assert(fixture.frame_hash==charge_start_hash[i]);
  }
  /* At either limit, additional taps preserve the last effect and valid cell. */
  for(size_t direction=0;direction<2;direction++) {
    uint32_t boundary_hash=0;
    for(size_t sample=0;sample<2;sample++) {
      fixture=(fixture_t){.inspector_layer=14,.tap_x=direction?250:110,.tap_y=355,
          .tap_count=sample?12u:(direction?2u:3u)};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="charge-cells",.time_ms="130"})==H2_PAL_OK);
      if(!sample)boundary_hash=fixture.frame_hash;
      else assert(boundary_hash==fixture.frame_hash);
    }
  }
  /* Real DOWN/MOVE/UP selection: wrap both directions, reject short drags.
   * Fixed visual offset isolates selection; the events still run real Lua input. */
  const char *selections[]={"0","1","2","3"};
  uint32_t selection_hash[4];
  for(int i=0;i<4;i++) {
    fixture=(fixture_t){.inspector_layer=17};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="1875",.selected=selections[i],.drag="0"})==H2_PAL_OK);
    selection_hash[i]=fixture.frame_hash;
    for(int j=0;j<i;j++)assert(selection_hash[i]!=selection_hash[j]);
  }
  const int distances[]={-60,60,-28,28};
  for(int i=0;i<4;i++)for(int d=0;d<4;d++)for(int skip=0;skip<2;skip++) {
    fixture=(fixture_t){.inspector_layer=17,.tap_x=184,.tap_y=410,.drag_distance=distances[d],.skip_move=skip};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="1875",.selected=selections[i],.drag="0"})==H2_PAL_OK);
    int expected=d==0?(i+1)%4:d==1?(i+3)%4:i;
    assert(fixture.frame_hash==selection_hash[expected]);
  }
  /* H106 renders at physical 240x240, with per-group uniform layout and
   * inverse-mapped touch hitboxes. Original AMOLED checks above still apply. */
  for(size_t layer=0;layer<sizeof(layers)/sizeof(layers[0]);layer++) {
    fixture=(fixture_t){.h106=1,.inspector_layer=(int)layer+1};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer=layers[layer],.time_ms="1875"})==H2_PAL_OK);
    assert(fixture.draw>=1u && fixture.present>=6u && fixture.display_close==1 && fixture.touch_close==1);
  }
  const int h106_taps[][2]={{25,22},{80,22},{150,22},{220,22},{70,172},{170,172}};
  for(int i=0;i<6;i++) {
    const char *layer=i<4?"hud":"charge-cells";
    fixture=(fixture_t){.h106=1,.inspector_layer=i<4?11:14};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer=layer,.time_ms="0",.health_fx=i<4?effects[i]:NULL,.charge_fx=i<4?NULL:i==4?"down":"up"})==H2_PAL_OK);
    uint32_t probe=fixture.frame_hash;
    fixture=(fixture_t){.h106=1,.inspector_layer=i<4?11:14,.tap_x=h106_taps[i][0],.tap_y=h106_taps[i][1]};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer=layer,.time_ms="0"})==H2_PAL_OK);
    assert(fixture.frame_hash==probe);
  }
  for(int direction=0;direction<2;direction++) {
    fixture=(fixture_t){.h106=1,.inspector_layer=17};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.selected=direction?"3":"1",.drag="0"})==H2_PAL_OK);
    uint32_t probe=fixture.frame_hash;
    fixture=(fixture_t){.h106=1,.inspector_layer=17,.tap_x=120,.tap_y=215,.drag_distance=direction?50:-50};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.drag="0"})==H2_PAL_OK);
    assert(fixture.frame_hash==probe);
  }
  /* Upward icon release and cancel cases use the same PAL touch path as SDL
   * and firmware, including fast DOWN/UP without a MOVE. */
  for(int layout=0;layout<2;layout++) {
    fixture=(fixture_t){.h106=layout,.inspector_layer=17};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.drag="0"})==H2_PAL_OK);
    uint32_t idle=fixture.frame_hash;
    for(int kind=0;kind<4;kind++)for(int skip=0;skip<2;skip++) {
      fixture=(fixture_t){.h106=layout,.inspector_layer=17,.tap_x=layout?120:184,
        .tap_y=layout?209:400,.drag_dy=layout?-45:-75,.skip_move=skip};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel",.time_ms="0",.selected=selections[kind],.drag="0"})==H2_PAL_OK);
      assert(fixture.frame_hash!=idle);
    }
    for(int kind=0;kind<3;kind++) {
      /* Too short, downward, and upward from outside an icon: never cast. */
      fixture=(fixture_t){.h106=layout,.inspector_layer=17,
        .tap_x=kind==2?5:layout?120:184,.tap_y=layout?209:400,
        .drag_dy=kind==0?-8:kind==1?12:-65};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel",.time_ms="0",.drag="0"})==H2_PAL_OK);
      assert(fixture.frame_hash==idle);
    }
    fixture=(fixture_t){.h106=layout,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="1800"})==H2_PAL_OK);
    uint32_t recovered=fixture.frame_hash;
    const char *actions[]={"charge","wave","absorb","guard"};
    for(int kind=0;kind<4;kind++) {
      uint32_t peak=0;
      const char *times[]={"650","770","1800"};
      for(int sample=0;sample<3;sample++) {
        fixture=(fixture_t){.h106=layout,.inspector_layer=22};
        assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
          .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
          .layer="full",.time_ms=times[sample],.action=actions[kind]})==H2_PAL_OK);
        if(sample==0)peak=fixture.frame_hash;
        if(sample==1)assert(peak!=fixture.frame_hash);
        if(sample==2)assert(recovered==fixture.frame_hash);
      }
    }
  }
  /* Desktop clicks: physical left/right rotation, center cast, drag cancellation.
   * Firmware drag controls above remain unchanged. */
  for(int h106=0;h106<=1;h106++) {
    uint32_t idle[4];
    for(int skill=0;skill<4;skill++) {
      fixture=(fixture_t){.h106=h106,.inspector_layer=17};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel",.time_ms="0",.selected=selections[skill],.drag="0",.click_controls=1})==H2_PAL_OK);
      idle[skill]=fixture.frame_hash;
    }
    for(int skill=0;skill<4;skill++)for(int right=0;right<2;right++) {
      fixture=(fixture_t){.h106=h106,.inspector_layer=17,.tap_count=1,
        .tap_x=h106?(right?185:55):(right?292:76),.tap_y=h106?217:414};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel",.time_ms="0",.selected=selections[skill],.drag="0",.click_controls=1})==H2_PAL_OK);
      assert(fixture.frame_hash==idle[(skill+(right?3:1))%4]);
    }
    uint32_t cast_hash=0;
    for(int repeat=0;repeat<2;repeat++) {
      fixture=(fixture_t){.h106=h106,.inspector_layer=17,.tap_count=repeat?2:1,
        .tap_x=h106?120:184,.tap_y=h106?209:400};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel",.time_ms="0",.drag="0",.click_controls=1})==H2_PAL_OK);
      assert(fixture.frame_hash!=idle[0]);
      if(!repeat)cast_hash=fixture.frame_hash;
      else assert(cast_hash==fixture.frame_hash); /* Busy clicks never restart the echo. */
    }
    fixture=(fixture_t){.h106=h106,.inspector_layer=17,.drag_distance=60,
      .tap_x=h106?120:184,.tap_y=h106?209:400};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.drag="0",.click_controls=1})==H2_PAL_OK);
    assert(fixture.frame_hash==idle[0]);
  }
  /* The default game is a single click-to-pair intro. Fixed-time game probes
   * bypass the real-time intro so combat selection tests remain deterministic. */
  uint32_t game_unlocked_hash=0;
  for(int h106=0;h106<=1;h106++) {
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="0",.battle=1,.click_controls=1})==H2_PAL_OK);
    if(h106)game_unlocked_hash=fixture.frame_hash;

    fixture=(fixture_t){.h106=h106,.inspector_layer=22,.intro_probe=1};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.battle=1,.click_controls=1})==H2_PAL_OK);
    uint32_t waiting_hash=fixture.frame_hash;
    fixture=(fixture_t){.h106=h106,.inspector_layer=22,.intro_probe=1,.tap_count=1,
      .tap_x=h106?120:184,.tap_y=h106?120:224};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.battle=1,.click_controls=1})==H2_PAL_OK);
    assert(waiting_hash!=fixture.frame_hash);
  }
  /* Confirmed glass state differs from candidate and rejects side clicks. */
  uint32_t locked_hash=0;
  for(int probe=1;probe<=3;probe++) {
    fixture=(fixture_t){.h106=1,.inspector_layer=22,.game_lock_probe=probe};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="0",.battle=1,.click_controls=1})==H2_PAL_OK);
    if(probe==1)locked_hash=fixture.frame_hash;
    else assert(fixture.frame_hash==locked_hash); /* side clicks cannot change the confirmed move */
    assert(fixture.frame_hash!=game_unlocked_hash);
  }
  /* Full charge must not animate the base, in either screen profile. */
  for(int h106=0;h106<=1;h106++)for(int full=0;full<=1;full++) {
    uint32_t first=0;
    for(int sample=0;sample<2;sample++) {
      fixture=(fixture_t){.h106=h106,.inspector_layer=13};
      assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .layer="carousel-frame",.qi=full?"5":"4",.time_ms=sample?"240":"0"})==H2_PAL_OK);
      if(!sample)first=fixture.frame_hash;
      else assert(first==fixture.frame_hash);
    }
  }
  /* Beam-clash close-ups render on both layouts and the combo push reaches a
   * visibly different final composition from an equal-power midpoint. */
  for(int h106=0;h106<=1;h106++) {
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="420",.clash="equal"})==H2_PAL_OK);
    uint32_t equal_clash=fixture.frame_hash;
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="820",.clash="player-combo"})==H2_PAL_OK);
    assert(fixture.frame_hash!=equal_clash);

    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="1080",.clash="equal"})==H2_PAL_OK);
    assert(fixture.frame_hash!=equal_clash); /* Reviewed four-frame fade reaches black. */

    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="240",.result="win"})==H2_PAL_OK);
    uint32_t sliding_result=fixture.frame_hash;
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="640",.result="win"})==H2_PAL_OK);
    uint32_t lit_result=fixture.frame_hash;
    assert(lit_result!=sliding_result); /* Split words arrive before lighting. */
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="640",.result="lose"})==H2_PAL_OK);
    assert(fixture.frame_hash!=lit_result);

    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="1200",.result="win"})==H2_PAL_OK);
    uint32_t moving_result=fixture.frame_hash;
    assert(moving_result!=lit_result); /* Directional bars keep passing after word light-up. */
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="1600",.result="win"})==H2_PAL_OK);
    assert(fixture.frame_hash!=moving_result); /* The light train remains in motion. */
    fixture=(fixture_t){.h106=h106,.inspector_layer=22};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="full",.time_ms="2600",.result="win"})==H2_PAL_OK);
    assert(fixture.frame_hash!=moving_result); /* Independent speeds do not collapse into one tile loop. */
  }
  h2_runtime_deinit(runtime);
  static const h2_runtime_component_mapper_vtable_t button_mapper_vtable={button_map_list,button_map_get};
  static const h2_runtime_component_mapper_t button_mapper={NULL,&button_mapper_vtable};
  static const h2_pal_periph_vtable_t button_periph_vtable={.list=button_periph_list,.get=button_periph_get};
  static const h2_pal_periph_api_t button_periph={.vtable=&button_periph_vtable};
  runtime_config.component_mapper=&button_mapper;
  runtime_config.periph=&button_periph;
  assert(h2_runtime_init(&runtime_config,&runtime)==H2_PAL_OK);
  assert(h2_runtime_input_start(runtime,NULL)==H2_PAL_OK);
  h2_runtime_test_control_t *control=NULL;
  assert(h2_runtime_test_control_open(runtime,&control)==H2_PAL_OK);
  for(unsigned id=9;id<=11;id++) {
    fixture=(fixture_t){.h106=1,.inspector_layer=17};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.selected=id==9?"1":id==10?"3":"0",.drag="0"})==H2_PAL_OK);
    uint32_t expected=fixture.frame_hash;
    fixture=(fixture_t){.h106=1,.no_touch=1,.inspector_layer=17,.button_control=control,.button_id=id,.button_burst=id==9?24:0};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
      .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
      .layer="carousel",.time_ms="0",.drag="0"})==H2_PAL_OK);
    assert(fixture.button_sent);
    assert(fixture.ready && fixture.present >= 6 && fixture.touch_close == 0);
    if(id<11)assert(fixture.frame_hash==expected);
    else assert(fixture.frame_hash!=expected);
  }
  /* On the button-only H106, only Record dismisses the result screen. */
  uint32_t result_hash=0;
  for(unsigned id=8;id<=11;id++) {
    fixture=(fixture_t){.h106=1,.no_touch=1,.inspector_layer=22,.game_lock_probe=1,.result_key_probe=1,
        .button_control=control,.button_id=id==8?0:id};
    assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
        .should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
        .battle=1,.result="win",.time_ms="5000",.capture_step_ms="33"})==H2_PAL_OK);
    if(id==8)result_hash=fixture.frame_hash;
    else if(id<11)assert(fixture.frame_hash==result_hash);
    else assert(fixture.frame_hash!=result_hash);
  }
#ifdef H2_QI_DUEL_DESKTOP_VECTORS
  /* Approved hand review switches must agree with the default renderer. */
  for(int component=0;component<2;component++)for(int h106=0;h106<=1;h106++) {
    for(int layer=0;layer<3;layer++) {
      const char *name=layer==0?"hand-left":layer==1?"hand-right":"hud";
      uint32_t baseline=0;
      for(int candidate=0;candidate<2;candidate++) {
        fixture=(fixture_t){.h106=h106,.inspector_layer=22};
        assert(h2_lua_qi_duel_run(runtime,&(h2_lua_qi_duel_config_t){
          .back_component_id=H2_RUNTIME_COMPONENT_ID_NONE,.should_stop=should_stop,.should_stop_user=&fixture,.on_ready=ready,.on_ready_user=&fixture,
          .layer=name,.time_ms="250",.draw_component=candidate?(component?"hand-right":"hand-left"):NULL})==H2_PAL_OK);
        if(!candidate)baseline=fixture.frame_hash;
        else assert(fixture.frame_hash==baseline);
      }
    }
  }
#endif
  h2_runtime_test_control_close(control);
  h2_runtime_deinit(runtime);
  return 0;
}
