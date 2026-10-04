/* Real command, serial observer and KCP code; only UART/clock are injected. */
#include "h2_h2loader_host_serial.c"
#include <assert.h>
#include <stdlib.h>

typedef struct fixture {
    uint64_t now;
    unsigned closes, baud_restores, handshakes, reboots, writes_after_ready;
    int mode, ready_sent;
    uint8_t input[4096];
    size_t input_length, input_offset;
    char expected[131072], observed[131072];
    size_t expected_length, observed_length;
    h2_iostreamikcp_t *peer;
    h2_iostreamikcp_filter_t peer_filter;
} fixture_t;
static void *allocate(void *u, size_t n) { (void)u; return malloc(n); }
static void *resize(void *u, void *p, size_t n) { (void)u; return realloc(p,n); }
static void dispose(void *u, void *p) { (void)u; free(p); }
static const h2_pal_mem_vtable_t mem_vtable = {.alloc=allocate,.realloc=resize,.free=dispose};
static const h2_pal_mem_api_t mem = {.vtable=&mem_vtable};
static int clock_ms(void *u, uint64_t *n) { *n=((fixture_t *)u)->now; return 0; }
static uint32_t peer_now(void *u) { return (uint32_t)((fixture_t *)u)->now; }
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms=clock_ms};
static void expect(fixture_t *f, const void *p, size_t n) {
    assert(n<=sizeof(f->expected)-f->expected_length);
    memcpy(f->expected+f->expected_length,p,n); f->expected_length+=n;
}
static int log_bytes(void *u, const uint8_t *p, size_t n) {
    fixture_t *f=u;
    if (f->mode==2 && f->now>=800u) return H2_PAL_ERR_IO;
    assert(n<=sizeof(f->observed)-f->observed_length);
    memcpy(f->observed+f->observed_length,p,n); f->observed_length+=n; return 0;
}
static int peer_output(void *u, const void *p, size_t n, size_t *written, uint32_t timeout) {
    fixture_t *f=u; (void)timeout;
    assert(n<=sizeof(f->input)-f->input_length);
    memcpy(f->input+f->input_length,p,n); f->input_length+=n; *written=n; return 0;
}
static int peer_frame(void *u, const h2_iostreamikcp_frame_t *frame) {
    fixture_t *f=u;
    if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) { ++f->handshakes; return 0; }
    if (frame->flags!=H2_IOSTREAMIKCP_FRAME_FLAG_DATA) return 0;
    assert(h2_iostreamikcp_input_frame(f->peer,frame)==0);
    uint8_t command[128]; size_t n=0;
    int rc=h2_iostreamikcp_read(f->peer,command,sizeof(command),&n);
    if (rc==0 && n>0) {
        const char wanted[]="h2loader reboot loader\n";
        assert(n==sizeof(wanted)-1u && memcmp(command,wanted,n)==0);
        ++f->reboots;
        const char *response=f->mode==3 ?
            "H2_LOADER_REBOOT target=loader result=fail code=-4\n" :
            "H2_LOADER_REBOOT target=loader result=accepted\n"
            "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\n";
        assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)response,strlen(response))==0);
    } else assert(rc==H2_PAL_ERR_WOULD_BLOCK || (rc==0 && n==0));
    return h2_iostreamikcp_flush(f->peer);
}
static int uart_write(void *u, const void *p, size_t n, size_t *written, uint32_t timeout) {
    fixture_t *f=u; (void)timeout; *written=n;
    if (f->ready_sent) ++f->writes_after_ready;
    return h2_iostreamikcp_filter_input(&f->peer_filter,p,n,peer_frame,f);
}
static int uart_read(void *u, void *out, size_t capacity, size_t *read, uint32_t timeout) {
    fixture_t *f=u; (void)timeout; f->now+=10u; *read=0;
    if (f->mode==1 && f->now>=800u) return H2_PAL_ERR_CLOSED;
    if (f->input_offset==f->input_length) {
        f->input_offset=0; f->input_length=0;
        if (!f->ready_sent && f->now>=500u && f->mode!=3) {
            const char ready[]="H2_LOADER_READY target=bk status=ready\r\n";
            f->ready_sent=1;
            expect(f,ready,sizeof(ready)-1u);
            if (f->mode==4) {
                assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)ready,sizeof(ready)-1u)==0);
                assert(h2_iostreamikcp_flush(f->peer)==0);
            } else {
                memcpy(f->input,ready,sizeof(ready)-1u); f->input_length=sizeof(ready)-1u;
            }
            /* Same conversation ID after READY is still an untrusted epoch.
             * Never decode its stale console text or send a KCP ACK for it. */
            const char stale[]="STALE-CONVERSATION-MUST-NOT-LEAK\r\n";
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)stale,sizeof(stale)-1u)==0);
            assert(h2_iostreamikcp_flush(f->peer)==0);
            const char tail[]="native-after-reset\r\n";
            memcpy(f->input+f->input_length,tail,sizeof(tail)-1u); f->input_length+=sizeof(tail)-1u;
            expect(f,tail,sizeof(tail)-1u);
        } else {
            int n=snprintf((char *)f->input,sizeof(f->input),"native-record-%llu\r\n",(unsigned long long)f->now);
            assert(n>0); f->input_length=(size_t)n; expect(f,f->input,f->input_length);
        }
    }
    *read=f->input_length-f->input_offset;
    if (*read>capacity) *read=capacity;
    /* Split READY, the stale frame header and following raw text across reads. */
    if (f->ready_sent && *read>13u) *read=13u;
    memcpy(out,f->input+f->input_offset,*read); f->input_offset+=*read; return 0;
}
static int close_uart(void *u, h2_pal_serial_host_session_t **session) {
    fixture_t *f=u; assert(*session==(h2_pal_serial_host_session_t *)f);
    ++f->closes; ++f->baud_restores; *session=NULL; return 0;
}
static const h2_pal_serial_host_vtable_t serial_vtable = {.close=close_uart};
static int uart_flush(void *u) { (void)u; return 0; }
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {.read=uart_read,.write=uart_write,.flush=uart_flush};
static int cancelled(void *u) {
    fixture_t *f=u;
    return f->now>=21000u && f->input_offset==f->input_length;
}
static void run_case(int mode) {
    fixture_t f={.mode=mode};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_uart_io_stream_api_t uart={.user=&f,.vtable=&uart_vtable};
    h2_h2loader_host_serial_connection_t *c=calloc(1,sizeof(*c)); assert(c!=NULL);
    *c=(h2_h2loader_host_serial_connection_t){.serial=&serial,.session=(h2_pal_serial_host_session_t *)&f,
        .uart=&uart,.allocator=&mem,.time=&time,.on_log=log_bytes,.log_user=&f,
        .conversation_id=42u,.command_timeout_ms=1000u};
    h2_iostreamikcp_config_t host={.io=h2_iostreamikcp_io_from_uart(&uart),.allocator=&mem,
        .conv=42u,.now_ms=serial_stream_now_ms,.time_user=c,.rx_buffer_size=65536u,
        .on_log=serial_stream_log,.log_user=c};
    assert(h2_iostreamikcp_open(&host,&c->stream)==0);
    h2_iostreamikcp_config_t peer={.io={.user=&f,.write=peer_output},.allocator=&mem,
        .conv=42u,.now_ms=peer_now,.time_user=&f,.rx_buffer_size=65536u};
    assert(h2_iostreamikcp_open(&peer,&f.peer)==0); h2_iostreamikcp_filter_init(&f.peer_filter);
    assert(h2_h2loader_host_serial_monitor_continuous(c,NULL,&f)==H2_PAL_ERR_INVALID_ARG);
    assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_ERR_INVALID_STATE);
    h2_h2loader_host_status_t status={.active_role=H2_H2LOADER_HOST_ACTIVE_ROLE_APP,
        .command_availability=H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL,.mfg_mode=1u};
    h2_h2loader_host_command_request_t request={.command=H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER,.status=&status};
    h2_h2loader_host_command_result_t result={0};
    int command_rc=h2_h2loader_host_serial_execute_command(c,&request,&result);
    assert(command_rc==(mode==3?H2_PAL_ERR_IO:H2_PAL_OK));
    assert(f.reboots==1u && !f.closes && !f.baud_restores);
    if (mode==3) {
        assert(result.terminal==H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR);
        assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_ERR_INVALID_STATE);
    } else {
        assert(result.terminal==H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
        assert(f.input_offset==f.input_length);
        const char framed[]="framed-console-before-reset\r\n";
        assert(h2_iostreamikcp_write(f.peer,(const uint8_t *)framed,sizeof(framed)-1u)==0);
        assert(h2_iostreamikcp_flush(f.peer)==0); expect(&f,framed,sizeof(framed)-1u);
        /* A physical frame prefix buffered before the ownership transition
         * must survive detach, proving byte continuity rather than a new filter. */
        assert(f.input_length-f.input_offset>3u);
        assert(h2_iostreamikcp_input(c->stream,f.input+f.input_offset,3u)==0); f.input_offset+=3u;
        int rc=h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f);
        assert(rc==(mode==1?H2_PAL_ERR_CLOSED:mode==2?H2_PAL_ERR_IO:H2_PAL_EXIT));
        assert(!c->stream && !c->conversation_id && !f.writes_after_ready && !f.handshakes);
        if (mode==0 || mode==4) {
            assert(f.now>=21000u);
            assert(f.expected_length==f.observed_length && memcmp(f.expected,f.observed,f.expected_length)==0);
        }
        assert(!c->accepted_reboot && !c->continuous_monitor);
        assert(h2_h2loader_host_serial_monitor_continuous(c,cancelled,&f)==H2_PAL_ERR_INVALID_STATE);
    }
    assert(!f.closes && !f.baud_restores);
    assert(h2_h2loader_host_serial_disconnect(&c)==0);
    assert(c==NULL && f.closes==1u && f.baud_restores==1u);
    h2_iostreamikcp_close(f.peer);
}
int main(void) { for (int mode=0;mode<5;++mode) run_case(mode); return 0; }
