#include "h2_h2loader_cli_app.h"
#include "h2_iostreamikcp.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEALTHY, OPEN_FAILURE, STREAM_FAILURE, HANDSHAKE_TIMEOUT, BAD_STATUS, CLOSE_FAILURE };
typedef struct scan_fixture {
    int mode;
    unsigned opens, streams, closes, controls, scans, snapshots_closed, requests;
    uint32_t modem_lines;
    uint64_t now;
    size_t allocations;
    uint8_t input[16384];
    size_t input_len, input_offset;
    char output[16384];
    size_t output_len;
    h2_iostreamikcp_t *peer;
    h2_iostreamikcp_filter_t filter;
    h2_pal_mem_api_t mem;
    h2_pal_uart_io_stream_api_t uart;
} scan_fixture_t;

static void *allocate(void *user, size_t size) {
    scan_fixture_t *f = user;
    void *p = malloc(size);
    if (p != NULL) ++f->allocations;
    return p;
}
static void *resize(void *user, void *p, size_t size) {
    if (p == NULL) return allocate(user, size);
    return realloc(p, size);
}
static void release(void *user, void *p) {
    if (p != NULL) { --((scan_fixture_t *)user)->allocations; free(p); }
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc=allocate, .realloc=resize, .free=release};
static int clock_ms(void *user, uint64_t *out) { *out=((scan_fixture_t *)user)->now; return H2_PAL_OK; }
static uint32_t peer_ms(void *user) { return (uint32_t)((scan_fixture_t *)user)->now; }
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms=clock_ms};
static int output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    scan_fixture_t *f=user; (void)timeout;
    assert(len < sizeof(f->output)-f->output_len);
    memcpy(f->output+f->output_len,data,len); f->output_len+=len;
    f->output[f->output_len]='\0'; *written=len; return H2_PAL_OK;
}
static int flush(void *user) { (void)user; return H2_PAL_OK; }
static const h2_command_io_vtable_t output_vtable = {.write=output, .flush=flush};

static int peer_output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    scan_fixture_t *f=user; (void)timeout;
    if (f->input_offset == f->input_len) f->input_offset=f->input_len=0u;
    assert(len <= sizeof(f->input)-f->input_len);
    memcpy(f->input+f->input_len,data,len); f->input_len+=len;
    *written=len; return H2_PAL_OK;
}
static void emit_status(scan_fixture_t *f) {
    const char *sha="abababababababababababababababababababababababababababababababab";
    char line[2048];
    int len=snprintf(line,sizeof(line),
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 device_uid=102030405060 "
        "capabilities=0x00000005 command_availability=0x00000008 "
        "active_role=app active_version=v1 active_checksum=%s active_image_size=4096 "
        "running_partition=2 next_partition=2 boot_intent=auto "
        "stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- "
        "stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=- "
        "partition_1_valid=1 partition_1_package_checksum=%s partition_1_package_size=3 "
        "partition_1_image_checksum=%s partition_1_image_size=2 partition_1_role=loader "
        "partition_1_version=v1 partition_1_board=devkit partition_1_target=esp32s3 "
        "partition_2_valid=1 partition_2_package_checksum=%s partition_2_package_size=3 "
        "partition_2_image_checksum=%s partition_2_image_size=2 partition_2_role=app "
        "partition_2_version=v1 partition_2_board=devkit partition_2_target=esp32s3 "
        "last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000\n",sha,sha,sha,sha,sha);
    assert(len>0 && (size_t)len<sizeof(line));
    if (f->mode==BAD_STATUS) strcpy(line,"H2_LOADER_STATUS invalid=1\n");
    assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)line,strlen(line))==H2_PAL_OK);
}
static int peer_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    scan_fixture_t *f=user;
    if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN ||
        frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE) {
        if (f->mode==HANDSHAKE_TIMEOUT) return H2_PAL_OK;
        if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
            assert(f->peer==NULL);
            h2_iostreamikcp_config_t config={.io={.user=f,.write=peer_output},.allocator=&f->mem,
                .conv=frame->conv,.now_ms=peer_ms,.time_user=f,.rx_buffer_size=4096u};
            assert(h2_iostreamikcp_open(&config,&f->peer)==H2_PAL_OK);
        }
        h2_iostreamikcp_frame_t ack=*frame; ack.flags=H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK;
        uint8_t encoded[32]; size_t len=0u, written=0u;
        assert(h2_iostreamikcp_frame_encode(&ack,encoded,sizeof(encoded),&len)==H2_PAL_OK);
        return peer_output(f,encoded,len,&written,0u);
    }
    assert(frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_DATA && f->peer!=NULL);
    int rc=h2_iostreamikcp_input_frame(f->peer,frame);
    if (rc!=H2_PAL_OK) return rc;
    uint8_t command[64]; size_t len=0u;
    rc=h2_iostreamikcp_read(f->peer,command,sizeof(command),&len);
    if (rc==H2_PAL_OK && len!=0u) {
        const char expected[]="h2loader status\n";
        assert(len==sizeof(expected)-1u && memcmp(command,expected,len)==0);
        ++f->requests; emit_status(f);
    } else assert(rc==H2_PAL_ERR_WOULD_BLOCK || (rc==H2_PAL_OK && len==0u));
    return h2_iostreamikcp_flush(f->peer);
}
static int uart_write(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    scan_fixture_t *f=user; (void)timeout; *written=len;
    return h2_iostreamikcp_filter_input(&f->filter,data,len,peer_frame,f);
}
static int uart_read(void *user, void *data, size_t cap, size_t *read, uint32_t timeout) {
    scan_fixture_t *f=user; f->now+=timeout!=0u?timeout:1u;
    *read=f->input_len-f->input_offset;
    if (*read>cap) *read=cap;
    if (*read>67u) *read=67u;
    if (*read==0u) return H2_PAL_ERR_TIMEOUT;
    memcpy(data,f->input+f->input_offset,*read); f->input_offset+=*read; return H2_PAL_OK;
}
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {.read=uart_read,.write=uart_write,.flush=flush};

static int scan(void *user, h2_pal_serial_host_snapshot_t **out) {
    scan_fixture_t *f=user; ++f->scans; *out=(h2_pal_serial_host_snapshot_t *)f; return H2_PAL_OK;
}
static int count(void *user, const h2_pal_serial_host_snapshot_t *snapshot, size_t *out) {
    assert(snapshot==(h2_pal_serial_host_snapshot_t *)user); *out=1u; return H2_PAL_OK;
}
static int port(void *user, const h2_pal_serial_host_snapshot_t *snapshot, size_t index, h2_pal_serial_host_port_info_t *out) {
    assert(snapshot==(h2_pal_serial_host_snapshot_t *)user && index==0u); memset(out,0,sizeof(*out));
    strcpy(out->port_id,"scan-port"); strcpy(out->endpoint,"/dev/mock-scan");
    out->capabilities=H2_PAL_SERIAL_HOST_CAP_DTR|H2_PAL_SERIAL_HOST_CAP_RTS;
    return H2_PAL_OK;
}
static int snapshot_close(void *user, h2_pal_serial_host_snapshot_t **snapshot) {
    scan_fixture_t *f=user; assert(*snapshot==(h2_pal_serial_host_snapshot_t *)f);
    ++f->snapshots_closed; *snapshot=NULL; return H2_PAL_OK;
}
static int open_port(void *user, const char *id, const h2_pal_uart_io_stream_config_t *config, h2_pal_serial_host_session_t **out) {
    scan_fixture_t *f=user; ++f->opens; assert(strcmp(id,"scan-port")==0 && config->baud_rate==460800u);
    if (f->mode==OPEN_FAILURE) return H2_PAL_ERR_BUSY;
    *out=(h2_pal_serial_host_session_t *)f; return H2_PAL_OK;
}
static int stream(void *user, h2_pal_serial_host_session_t *session, const h2_pal_uart_io_stream_api_t **out) {
    scan_fixture_t *f=user; assert(session==(h2_pal_serial_host_session_t *)f); ++f->streams;
    if (f->mode==STREAM_FAILURE) return H2_PAL_ERR_IO;
    *out=&f->uart; return H2_PAL_OK;
}
static int set_lines(void *user, h2_pal_serial_host_session_t *session, uint32_t mask, uint32_t lines) {
    scan_fixture_t *f=user; assert(session==(h2_pal_serial_host_session_t *)f); ++f->controls;
    f->modem_lines=(f->modem_lines&~mask)|(lines&mask); return H2_PAL_OK;
}
static int close_port(void *user, h2_pal_serial_host_session_t **session) {
    scan_fixture_t *f=user; assert(*session==(h2_pal_serial_host_session_t *)f);
    ++f->closes; *session=NULL;
    return f->mode==CLOSE_FAILURE?H2_PAL_ERR_IO:H2_PAL_OK;
}
static const h2_pal_serial_host_vtable_t serial_vtable = {.scan=scan,.snapshot_count=count,
    .snapshot_get=port,.snapshot_destroy=snapshot_close,.open=open_port,.session_stream=stream,
    .set_control_lines=set_lines,.close=close_port};

int main(void) {
    for (int mode=HEALTHY; mode<=CLOSE_FAILURE; ++mode) {
        scan_fixture_t f={.mode=mode,.modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
        f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
        f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
        h2_iostreamikcp_filter_init(&f.filter);
        h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
        h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
        h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
        h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
        const char *argv[]={"h2loader","--no-ble","scan","--probe-timeout","1"};
        h2_h2loader_cli_config_t config={.argc=5,.argv=argv,.serial=&serial,.stdout_io=&io,.stderr_io=&io};
        assert(h2_h2loader_cli_main(&runtime,&config)==H2_H2LOADER_CLI_EXIT_OK);
        assert(f.scans==1u && f.snapshots_closed==1u && f.opens==1u);
        assert(f.controls==0u);
        assert(f.modem_lines==(H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS));
        assert(f.closes==(mode==OPEN_FAILURE?0u:1u));
        assert(strstr(f.output,"\"port\": \"scan-port\"")!=NULL);
        if (mode==HEALTHY) {
            assert(f.requests==1u && strstr(f.output,"\"probe_result\": \"ok\"")!=NULL);
            assert(strstr(f.output,"\"device_uid\": \"102030405060\"")!=NULL);
        } else assert(strstr(f.output,"\"probe_result\": \"error\"")!=NULL);
        h2_iostreamikcp_close(f.peer);
        assert(f.allocations==0u);
    }
    return 0;
}
