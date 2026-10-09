#include "h2_mosaico_camera.h"
#include "mosaico_module_camera.h"
#include <assert.h>
static uint8_t bytes[16];
static int start_error, get_error, del_error, give_error;
static bool malformed, packed_stride;
static unsigned returns;
int mosaico_camera_new(const mosaico_camera_config_t *cfg, mosaico_camera_handle_t *p) {
    assert(cfg->slot == 0); *p = bytes; return ESP_OK;
}
int mosaico_camera_open(mosaico_camera_handle_t p) {(void)p;return ESP_OK;}
int mosaico_camera_start_stream(mosaico_camera_handle_t p) {(void)p;return start_error;}
int mosaico_camera_del(mosaico_camera_handle_t p) {(void)p;return del_error;}
int mosaico_camera_get_frame(mosaico_camera_handle_t p, mosaico_camera_frame_t *f) {
    (void)p;if(get_error)return get_error;
    *f=(mosaico_camera_frame_t){.data=bytes,.size=malformed?3:sizeof(bytes),.width=4,.height=2,.bytes_per_line=packed_stride?0:8,.pixel_format=V4L2_PIX_FMT_UYVY};return ESP_OK;
}
int mosaico_camera_return_frame(mosaico_camera_handle_t p,const mosaico_camera_frame_t *f) {
    (void)p;assert(f->data==bytes);++returns;return give_error;
}
void h2_test_camera_cpp_linkage(void);
int main(void) {
    h2_test_camera_cpp_linkage();
    const h2_pal_camera_api_t *a=h2_mosaico_camera();
    h2_pal_camera_frame_t f,other,old;
    assert(h2_pal_camera_start(NULL)==H2_PAL_ERR_UNSUPPORTED);
    const h2_pal_camera_vtable_t absent_ops = {0};
    const h2_pal_camera_api_t absent = {.vtable = &absent_ops};
    assert(h2_pal_camera_start(&absent)==H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_stop(&absent)==H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_stop(NULL)==H2_PAL_ERR_UNSUPPORTED);
    f.data = bytes;
    assert(h2_pal_camera_acquire(&absent,&f)==H2_PAL_ERR_UNSUPPORTED && !f.data);
    assert(h2_pal_camera_acquire(NULL,&f)==H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_release(&absent,&f)==H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_release(NULL,&f)==H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_release(a,NULL)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_camera_acquire(a,NULL)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_camera_acquire(a,&f)==H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_camera_start(a)==0);
    assert(h2_pal_camera_start(a)==H2_PAL_ERR_BUSY);
    assert(h2_pal_camera_acquire(a,&f)==0);old=f;
    assert(h2_pal_camera_acquire(a,&other)==H2_PAL_ERR_BUSY);
    assert(!other.data);
    assert(h2_pal_camera_stop(a)==H2_PAL_ERR_BUSY);
    give_error=ESP_ERR_TIMEOUT;
    assert(h2_pal_camera_release(a,&f)==H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_camera_stop(a)==H2_PAL_ERR_BUSY);
    give_error=0;
    assert(h2_pal_camera_release(a,&f)==0);
    assert(h2_pal_camera_release(a,&f)==H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_camera_acquire(a,&f)==0);
    assert(f.token!=old.token);
    assert(h2_pal_camera_release(a,&old)==H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_camera_release(a,&f)==0);
    packed_stride=true;
    assert(h2_pal_camera_acquire(a,&f)==0);
    assert(f.stride==8 && f.size==16);
    assert(h2_pal_camera_release(a,&f)==0);
    packed_stride=false;
    malformed=true;
    unsigned before=returns;
    assert(h2_pal_camera_acquire(a,&f)==H2_PAL_ERR_FORMAT);
    assert(returns==before+1 && !f.data);
    give_error=ESP_ERR_TIMEOUT;
    assert(h2_pal_camera_acquire(a,&f)==H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_camera_stop(a)==H2_PAL_ERR_TIMEOUT);
    give_error=0;
    assert(h2_pal_camera_stop(a)==0);
    assert(h2_pal_camera_start(a)==0);
    malformed=false;get_error=ESP_ERR_TIMEOUT;
    assert(h2_pal_camera_acquire(a,&f)==H2_PAL_ERR_TIMEOUT);
    get_error=0;del_error=ESP_ERR_TIMEOUT;
    assert(h2_pal_camera_stop(a)==H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_camera_start(a)==H2_PAL_ERR_BUSY);
    del_error=0;assert(h2_pal_camera_stop(a)==0);
    start_error=ESP_ERR_NOT_SUPPORTED;
    assert(h2_pal_camera_start(a)==H2_PAL_ERR_UNSUPPORTED);
    start_error=0;assert(h2_pal_camera_start(a)==0);
    assert(h2_pal_camera_stop(a)==0);
    assert(h2_pal_camera_stop(a)==0);
    return 0;
}
