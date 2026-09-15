#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h2_h2loader_host.h"
#include "h2_iostreamikcp.h"
/* FRAME */
typedef struct { unsigned char transport_log_line[4096]; size_t transport_log_line_len; unsigned transport_log_line_discard; const struct config *config; const struct runtime *runtime; } h2_h2loader_cli_context_t;
struct config { h2_h2loader_host_cancelled_fn is_cancelled; void *cancel_user; };
struct runtime { const h2_pal_time_api_t *time; };
static char output[65536];
static size_t output_len;
#define H2_H2LOADER_CLI_STREAM_STDOUT 1
static h2_pal_result_t h2_h2loader_cli_output_bytes(h2_h2loader_cli_context_t *ctx,int kind,const uint8_t *data,size_t n) {
    (void)ctx;
    (void)kind;
    assert(output_len+n<sizeof output);
    memcpy(output+output_len,data,n);
    output_len+=n;
    output[output_len]=0;
    return H2_PAL_OK;
}
/* OUTPUT */
/* Fake KCP delivery is irrelevant here: retain the real CRC/frame parser,
 * serial handshake and CLI text sink at their actual ownership boundaries. */
struct h2_iostreamikcp { h2_iostreamikcp_filter_t filter; h2_iostreamikcp_config_t config; };
h2_pal_result_t h2_iostreamikcp_open(const h2_iostreamikcp_config_t *config,h2_iostreamikcp_t **out) {
    *out=calloc(1,sizeof **out);
    assert(*out);
    (*out)->config=*config;
    return H2_PAL_OK;
}
void h2_iostreamikcp_close(h2_iostreamikcp_t *s) { free(s); }
h2_pal_result_t h2_iostreamikcp_input(h2_iostreamikcp_t *s,const uint8_t *data,size_t n) {
    return h2_iostreamikcp_filter_input_with_log(&s->filter,data,n,NULL,NULL,s->config.on_log,s->config.log_user);
}
h2_iostreamikcp_io_t h2_iostreamikcp_io_from_uart(const h2_pal_uart_io_stream_api_t *uart) {
    h2_iostreamikcp_io_t io={.user=(void *)uart};
    return io;
}
/* SERIAL */
static uint64_t now;
static uint8_t wire[2048];
static size_t wire_len,wire_pos,first_read,read_limit;
static unsigned reads,opens;
static int include_ready;
static h2_pal_result_t clock_ms(void *user,uint64_t *out) { (void)user; *out=now; return H2_PAL_OK; }
static h2_pal_result_t sleep_ms(void *user,uint32_t ms) { (void)user; now+=ms; return H2_PAL_OK; }
static const h2_pal_time_vtable_t time_vtable={.get_monotonic_ms=clock_ms,.sleep_ms=sleep_ms};
static const h2_pal_time_api_t timer={.vtable=&time_vtable};
static void *allocate(void *user,size_t n) { (void)user; return malloc(n); }
static void release(void *user,void *p) { (void)user; free(p); }
static const h2_pal_mem_vtable_t mem_vtable={.alloc=allocate,.free=release};
static const h2_pal_mem_api_t memory={.vtable=&mem_vtable};
static void append_frame(uint8_t flags,const uint8_t *data,size_t n) {
    const h2_iostreamikcp_frame_t frame={.flags=flags,.conv=7,.payload=data,.payload_len=n};
    size_t written=0;
    assert(h2_iostreamikcp_frame_encode(&frame,wire+wire_len,sizeof wire-wire_len,&written)==H2_PAL_OK);
    wire_len+=written;
}
static void prepare_wire(void) {
    wire_len=wire_pos=reads=0;
    if (include_ready) {
        const char ready[]="H2_LOADER_READY board=fake target=fake\r\n";
        memcpy(wire,ready,sizeof ready-1);
        wire_len=sizeof ready-1;
    }
    const uint8_t conv[]={7,0,0,0};
    append_frame(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK,conv,sizeof conv);
    const uint8_t payload[]={0,1,2,3,0,0,0,0,0,0,0,0};
    append_frame(H2_IOSTREAMIKCP_FRAME_FLAG_DATA,payload,sizeof payload);
    const char text[]="H2_PAL_E2E suite=64 case=13 result=0\r\n";
    memcpy(wire+wire_len,text,sizeof text-1);
    wire_len+=sizeof text-1;
}
static h2_pal_result_t uart_read(void *user,void *data,size_t n,size_t *out,uint32_t wait) {
    (void)user;
    now+=wait;
    size_t limit=reads++==0 ? first_read : read_limit;
    if (n>limit)
        n=limit;
    if (n>wire_len-wire_pos)
        n=wire_len-wire_pos;
    memcpy(data,wire+wire_pos,n);
    wire_pos+=n;
    *out=n;
    return n ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t uart_write(void *user,const void *data,size_t n,size_t *out,uint32_t wait) {
    (void)user; (void)data; (void)wait; *out=n; return H2_PAL_OK;
}
static h2_pal_result_t uart_flush(void *user) { (void)user; return H2_PAL_OK; }
static const h2_pal_uart_io_stream_vtable_t uart_vtable={.read=uart_read,.write=uart_write,.flush=uart_flush};
static const h2_pal_uart_io_stream_api_t uart={.vtable=&uart_vtable};
static h2_pal_result_t serial_open(void *user,const char *port,const h2_pal_uart_io_stream_config_t *config,h2_pal_serial_host_session_t **out) {
    (void)user; (void)port; (void)config;
    ++opens;
    prepare_wire();
    *out=(h2_pal_serial_host_session_t *)&uart;
    return H2_PAL_OK;
}
static h2_pal_result_t serial_stream(void *user,h2_pal_serial_host_session_t *session,const h2_pal_uart_io_stream_api_t **out) {
    (void)user; (void)session; *out=&uart; return H2_PAL_OK;
}
static h2_pal_result_t serial_close(void *user,h2_pal_serial_host_session_t **session) { (void)user; *session=NULL; return H2_PAL_OK; }
static const h2_pal_serial_host_vtable_t serial_vtable={.open=serial_open,.session_stream=serial_stream,.close=serial_close};
static const h2_pal_serial_host_api_t serial_api={.vtable=&serial_vtable};
static h2_h2loader_host_serial_connection_t *connection;
static h2_h2loader_cli_context_t context;
static h2_pal_result_t connect_serial(void) {
    const h2_h2loader_host_serial_connection_config_t config={.serial=&serial_api,.time=&timer,.allocator=&memory,.port_id="fake",.conversation_id=7,.handshake_timeout_ms=60000,.on_log=h2_h2loader_cli_transport_log,.log_user=&context};
    return h2_h2loader_host_serial_connect(&config,&connection);
}
static void drain(void) {
    while (wire_pos<wire_len) {
        uint8_t bytes[256];size_t n=0;
        assert(uart_read(NULL,bytes,sizeof bytes,&n,1)==H2_PAL_OK);
        assert(h2_iostreamikcp_input(connection->stream,bytes,n)==H2_PAL_OK);
    }
}
static void disconnect_serial(void) { h2_iostreamikcp_close(connection->stream);free(connection);connection=NULL; }
typedef struct { int unused; } h2_h2loader_cli_transport_t;
static h2_pal_result_t h2_h2loader_cli_transport_rediscover(h2_h2loader_cli_transport_t *t) { (void)t; return H2_PAL_OK; }
static int h2_h2loader_cli_reconnect_must_fail_closed(h2_h2loader_cli_transport_t *t,h2_pal_result_t rc) { (void)t;(void)rc;return 0; }
static h2_pal_result_t h2_h2loader_cli_transport_connect(h2_h2loader_cli_transport_t *t,h2_h2loader_host_status_t *out) {
    (void)t;
    h2_pal_result_t rc=connect_serial();
    if (rc!=H2_PAL_OK)
        return rc;
    drain();
    assert(strstr(output,"H2_LOADER_READY"));
    *out=(h2_h2loader_host_status_t){.active_role=H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,.running_partition=1,.next_partition=1,.boot_intent=H2_H2LOADER_HOST_BOOT_INTENT_LOADER,.last=H2_PAL_ERR_INVALID_STATE};
    return H2_PAL_OK;
}
static h2_pal_result_t h2_h2loader_cli_transport_disconnect(h2_h2loader_cli_transport_t *t) { (void)t;disconnect_serial();return H2_PAL_OK; }
/* VERIFY */
int main(int argc,char **argv) {
    assert(argc==2);
    static const struct config config={0};
    static const struct runtime runtime={.time=&timer};
    context.config=&config;context.runtime=&runtime;
    read_limit=256;first_read=256;
    if (!strcmp(argv[1],"frame_text") || !strcmp(argv[1],"split_frame_text")) {
        /* ACK and a prefix of a CRC-valid DATA frame share the handshake read.
         * Every subsequent split must retain the text immediately after it. */
        for (size_t split=23;split<52;++split) {
            first_read=split;
            read_limit=!strcmp(argv[1],"split_frame_text") ? 1 : 256;
            memset(&context,0,sizeof context);output_len=0;output[0]=0;
            assert(connect_serial()==H2_PAL_OK);
            drain();
            assert(!strcmp(output,"H2_PAL_E2E suite=64 case=13 result=0\r\n"));
            disconnect_serial();
        }
    } else if (!strcmp(argv[1],"reset_ready")) {
        include_ready=0;
        assert(connect_serial()==H2_PAL_OK);
        drain();
        output_len = 0;
        output[0] = 0;
        const char replay[]="[00:00:00.653]H2_LOADER_READY board=old target=old\r\n";
        for (size_t i=0;i<sizeof replay-1;++i) {
            assert(h2_iostreamikcp_input(connection->stream,(const uint8_t *)&replay[i],1)==H2_PAL_OK);
        }
        const char banner[]="H2_LOADER_READY board=fake target=fake\r\n";
        for (size_t i = 0; i < sizeof banner - 1; ++i) {
            h2_pal_result_t expected = i == sizeof banner - 2
                ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
            h2_pal_result_t rc = h2_iostreamikcp_input(
                connection->stream, (const uint8_t *)&banner[i], 1);
            assert(rc == expected);
        }
        assert(strstr(output, banner) != NULL);
        assert(strstr(output, replay) != NULL);
        disconnect_serial();
        include_ready=1;
        assert(connect_serial()==H2_PAL_OK);
        drain();
        assert(opens==2);
        disconnect_serial();
    } else if (!strcmp(argv[1], "ready_split")) {
        include_ready = 0;
        assert(connect_serial() == H2_PAL_OK);
        drain();
        output_len = 0;
        output[0] = 0;
        const char prefix[] = "H2_LOADER_READY bo";
        const char suffix[] = "ard=fake target=fake\r\n";
        assert(h2_iostreamikcp_input(connection->stream,
            (const uint8_t *)prefix, sizeof prefix - 1) == H2_PAL_OK);
        assert(h2_iostreamikcp_input(connection->stream,
            (const uint8_t *)suffix, sizeof suffix - 1) == H2_PAL_ERR_CLOSED);
        assert(strstr(output, "H2_LOADER_READY board=fake target=fake\r\n") != NULL);
        disconnect_serial();
    } else if (!strcmp(argv[1],"reconnect")) {
        include_ready=1;
        h2_h2loader_cli_transport_t transport={0};
        h2_h2loader_host_status_t before={.last=H2_PAL_ERR_INVALID_STATE},after={0};
        assert(reconnect_and_verify_reboot(&context,&transport,H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER,&before,&after)==H2_PAL_OK);
        assert(opens==1 && now<1000);
        disconnect_serial();
    } else if (!strcmp(argv[1],"upgrade_failure")) {
        h2_h2loader_host_status_t before={0},after={.last=H2_PAL_ERR_IO};
        assert(h2_h2loader_cli_verify_reboot_status(H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE,&before,&after)==H2_PAL_ERR_INVALID_STATE);
    } else {
        return 2;
    }
    return 0;
}
