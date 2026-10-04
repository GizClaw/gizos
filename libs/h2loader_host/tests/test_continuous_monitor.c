/* Exercise the real serial monitor implementation and real iKCP decoder.
 * Only the physical UART/clock are injected; this is a host transport test. */
#include "h2_h2loader_host_serial.c"
#include <assert.h>
#include <stdlib.h>

typedef struct fixture {
    uint64_t now;
    unsigned closes, opens, baud_restores, handshakes;
    uint8_t input[4096];size_t input_length,input_offset;
    char expected[131072],observed[131072];size_t expected_length,observed_length;
    h2_iostreamikcp_t *peer;
    h2_iostreamikcp_filter_t peer_filter;
} fixture_t;
static void *allocate(void *u,size_t n){(void)u;return malloc(n);}
static void *resize(void *u,void *p,size_t n){(void)u;return realloc(p,n);}
static void dispose(void *u,void *p){(void)u;free(p);}
static const h2_pal_mem_vtable_t mem_vtable={.alloc=allocate,.realloc=resize,.free=dispose};
static const h2_pal_mem_api_t mem={.vtable=&mem_vtable};
static int clock_ms(void *u,uint64_t *n){*n=((fixture_t *)u)->now;return 0;}
static uint32_t peer_now(void *u){return (uint32_t)((fixture_t *)u)->now;}
static const h2_pal_time_vtable_t time_vtable={.get_monotonic_ms=clock_ms};
static int log_bytes(void *u,const uint8_t *p,size_t n){
    fixture_t *f=u;assert(n<=sizeof(f->observed)-f->observed_length);
    memcpy(f->observed+f->observed_length,p,n);f->observed_length+=n;return 0;
}
static int peer_output(void *u,const void *p,size_t n,size_t *written,uint32_t timeout){
    fixture_t *f=u;(void)timeout;assert(n<=sizeof(f->input)-f->input_length);
    memcpy(f->input+f->input_length,p,n);f->input_length+=n;*written=n;return 0;
}
static int peer_frame(void *u,const h2_iostreamikcp_frame_t *frame){
    fixture_t *f=u;
    if(frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN){++f->handshakes;return 0;}
    return frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_DATA?h2_iostreamikcp_input_frame(f->peer,frame):0;
}
static int uart_write(void *u,const void *p,size_t n,size_t *written,uint32_t timeout){
    fixture_t *f=u;(void)timeout;*written=n;
    return h2_iostreamikcp_filter_input(&f->peer_filter,p,n,peer_frame,f);
}
static int uart_read(void *u,void *out,size_t capacity,size_t *read,uint32_t timeout){
    fixture_t *f=u;(void)timeout;f->now+=10u;
    if(f->input_offset==f->input_length){
        f->input_offset=0;f->input_length=0;
        int n=f->now==500u?snprintf((char *)f->input,sizeof(f->input),"H2_LOADER_READY target=bk status=ready\r\n"):
            snprintf((char *)f->input,sizeof(f->input),"native-record-%llu\r\n",(unsigned long long)f->now);
        assert(n>0);f->input_length=(size_t)n;
        assert(f->input_length<=sizeof(f->expected)-f->expected_length);
        memcpy(f->expected+f->expected_length,f->input,f->input_length);f->expected_length+=f->input_length;
    }
    *read=f->input_length-f->input_offset;
    if(*read>capacity)*read=capacity;
    memcpy(out,f->input+f->input_offset,*read);f->input_offset+=*read;return 0;
}
static int close_uart(void *u,h2_pal_serial_host_session_t **session){
    fixture_t *f=u;assert(*session==(h2_pal_serial_host_session_t *)f);
    ++f->closes;++f->baud_restores;*session=NULL;return 0;
}
static const h2_pal_serial_host_vtable_t serial_vtable={.close=close_uart};
static const h2_pal_uart_io_stream_vtable_t uart_vtable={.read=uart_read,.write=uart_write};
static int cancelled(void *u){return ((fixture_t *)u)->now>=21000u;}
int main(void){
    fixture_t f={0};h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_uart_io_stream_api_t uart={.user=&f,.vtable=&uart_vtable};
    h2_h2loader_host_serial_connection_t *c=calloc(1,sizeof(*c));assert(c!=NULL);
    *c=(h2_h2loader_host_serial_connection_t){.serial=&serial,.session=(h2_pal_serial_host_session_t *)&f,
        .uart=&uart,.allocator=&mem,.time=&time,.on_log=log_bytes,.log_user=&f,.conversation_id=42u};
    h2_iostreamikcp_config_t host={.io=h2_iostreamikcp_io_from_uart(&uart),.allocator=&mem,
        .conv=42u,.now_ms=serial_stream_now_ms,.time_user=c,.rx_buffer_size=65536u,.on_log=serial_stream_log,.log_user=c};
    assert(h2_iostreamikcp_open(&host,&c->stream)==0);
    h2_iostreamikcp_config_t peer={.io={.user=&f,.write=peer_output},.allocator=&mem,
        .conv=42u,.now_ms=peer_now,.time_user=&f,.rx_buffer_size=65536u};
    assert(h2_iostreamikcp_open(&peer,&f.peer)==0);h2_iostreamikcp_filter_init(&f.peer_filter);
    assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_ERR_INVALID_STATE);
    assert(f.now==0 && !f.closes && !f.baud_restores);
    const char framed[]="framed-console-before-reset\r\n";
    assert(h2_iostreamikcp_write(f.peer,(const uint8_t *)framed,sizeof(framed)-1u)==0);
    assert(h2_iostreamikcp_flush(f.peer)==0);
    memcpy(f.expected,framed,sizeof(framed)-1u);f.expected_length=sizeof(framed)-1u;
    /* A prior accepted reboot is the required API admission. The simulated MCU
     * emits READY at 500ms and refuses command handshakes throughout 20s.
     * Native text continues across the old reconnect's 500ms blind window. */
    c->accepted_reboot=1;
    assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_EXIT);
    assert(f.now>=21000u && !f.closes && !f.baud_restores && !f.opens && !f.handshakes);
    assert(f.expected_length==f.observed_length && memcmp(f.expected,f.observed,f.expected_length)==0);
    assert(!c->accepted_reboot && !c->continuous_monitor);
    assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_ERR_INVALID_STATE);
    /* Physical close/baud restoration is owned by the caller only after cancel. */
    (void)h2_h2loader_host_serial_disconnect(&c);
    assert(c==NULL && f.closes==1u && f.baud_restores==1u);
    h2_iostreamikcp_close(f.peer);return 0;
}
