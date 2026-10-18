#include "h2_h2loader_cli_app.h"
#include "h2_iostreamikcp.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEALTHY, OPEN_FAILURE, STREAM_FAILURE, HANDSHAKE_TIMEOUT, BAD_STATUS, CLOSE_FAILURE, WRONG_REBOOT_ROLE };
enum { CLI_SCAN, CLI_STATUS, CLI_MONITOR, CLI_REBOOT };
static const char mixed_boot[] = "H2_DISPLAY_BOOT version=test\r\n";
static const char mixed_raw[] = "H2_DISPLAY_CASE first=PASS\r\n";
static const char mixed_tail[] = "H2_LOADER_STATUX diagnostic\r\nH2_DISPLAY_READY rc=0 confirm=0\r\n";
static const char connect_ready[] = "H2_LOADER_READY board=devkit target=esp32s3\r\n";
static const char after_ready[] = "H2_DISPLAY_CASE after-ready=PASS\r\n";
static const char mixed_expected[] = "H2_DISPLAY_BOOT version=test\r\n"
    "H2_DISPLAY_CASE first=PASS\r\n"
    "H2_LOADER_STATUX diagnostic\r\nH2_DISPLAY_READY rc=0 confirm=0\r\n";
typedef struct confirmation_probe {
    unsigned confirm_at, cancel_at, bad_at, bad_from, uid_from, sleep_fail_at;
    unsigned polls, stage_valid, cancel_seen;
    int mismatched_stage;
} confirmation_probe_t;
typedef struct cli_fixture {
    int mode;
    confirmation_probe_t *confirmation;
    const char *status_diagnostic;
    const char *status_eol_tail;
    const char *pre_ack_log;
    size_t mixed_read_size, status_prefix_split;
    int mixed_status_output, connect_output, connect_ready_mode;
    int status_eol_test, status_crlf, log_error_byte;
    size_t before_status_output_len;
    unsigned opens, streams, closes, controls, scans, snapshots_closed, requests;
    unsigned reboots, sleeps, cancel_after_opens, cancel_closes;
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
} cli_fixture_t;

static void *allocate(void *user, size_t size) {
    cli_fixture_t *f = user;
    void *p = malloc(size);
    if (p != NULL) ++f->allocations;
    return p;
}
static void *resize(void *user, void *p, size_t size) {
    if (p == NULL) return allocate(user, size);
    return realloc(p, size);
}
static void release(void *user, void *p) {
    if (p != NULL) { --((cli_fixture_t *)user)->allocations; free(p); }
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc=allocate, .realloc=resize, .free=release};
static int clock_ms(void *user, uint64_t *out) { *out=((cli_fixture_t *)user)->now; return H2_PAL_OK; }
static uint32_t peer_ms(void *user) { return (uint32_t)((cli_fixture_t *)user)->now; }
static int sleep_ms(void *user, uint32_t duration) {
    cli_fixture_t *f=user; ++f->sleeps; f->now+=duration;
    if (f->confirmation != NULL) {
        assert(duration==500u);
        if (f->confirmation->sleep_fail_at==f->sleeps) return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms=clock_ms,.sleep_ms=sleep_ms};
static int cancelled(void *user) {
    cli_fixture_t *f=user;
    if (f->confirmation != NULL) {
        confirmation_probe_t *p=f->confirmation;
        if (p->cancel_at!=0u && p->polls>=p->cancel_at) {
            p->cancel_seen=1u;
            return 1;
        }
        return p->polls>=p->confirm_at && !p->stage_valid;
    }
    if (f->mixed_status_output && f->output_len<sizeof(mixed_expected)-1u && f->now<20000u)
        return 0;
    if (f->cancel_after_opens!=0u && f->opens>=f->cancel_after_opens) {
        f->cancel_closes=f->closes; return 1;
    }
    return 0;
}
static int output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout;
    assert(len < sizeof(f->output)-f->output_len);
    memcpy(f->output+f->output_len,data,len); f->output_len+=len;
    if (f->connect_output && f->requests==0u) f->before_status_output_len+=len;
    f->output[f->output_len]='\0'; *written=len; return H2_PAL_OK;
}
static h2_pal_result_t connect_log(void *user, const uint8_t *data, size_t len) {
    cli_fixture_t *f=user;
    const uint8_t *error=f->log_error_byte?memchr(data,f->log_error_byte,len):NULL;
    size_t accepted=error?(size_t)(error-data):len, written=0u;
    if (accepted!=0u) {
        int rc=output(user,data,accepted,&written,0u);
        assert(rc==H2_PAL_OK && written==accepted);
    }
    return error?H2_PAL_ERR_IO:H2_PAL_OK;
}
static int cancel_after_one_poll(void *user) {
    unsigned *remaining=user;
    return (*remaining)--==0u;
}
static int flush(void *user) { (void)user; return H2_PAL_OK; }
static const h2_command_io_vtable_t output_vtable = {.write=output, .flush=flush};

static int peer_output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout;
    if (f->input_offset == f->input_len) f->input_offset=f->input_len=0u;
    assert(len <= sizeof(f->input)-f->input_len);
    memcpy(f->input+f->input_len,data,len); f->input_len+=len;
    *written=len; return H2_PAL_OK;
}
static void emit_status(cli_fixture_t *f) {
    const char *sha="abababababababababababababababababababababababababababababababab";
    int loader=f->reboots!=0u && f->mode!=WRONG_REBOOT_ROLE;
    char probe_stage[512];
    const char *stage="stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=-";
    if (f->confirmation != NULL) {
        confirmation_probe_t *p=f->confirmation;
        if (f->reboots!=0u) ++p->polls;
        p->stage_valid=f->reboots==0u || p->polls<p->confirm_at;
        loader=0;
        if (p->stage_valid) {
            int n=snprintf(probe_stage,sizeof(probe_stage),
                "stage_valid=1 stage_package_checksum=%s stage_package_size=3 stage_image_checksum=%s "
                "stage_image_size=2 stage_role=app stage_version=%s stage_board=devkit stage_target=esp32s3",
                sha,sha,p->mismatched_stage && f->reboots!=0u?"other":"v1");
            assert(n>0 && (size_t)n<sizeof(probe_stage)); stage=probe_stage;
        }
        if (f->reboots!=0u && (p->bad_at==p->polls || (p->bad_from!=0u && p->polls>=p->bad_from))) {
            const char bad[]="H2_LOADER_STATUS invalid=1\n";
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)bad,sizeof(bad)-1u)==H2_PAL_OK);
            return;
        }
    }
    char line[2048];
    int len=snprintf(line,sizeof(line),
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 device_uid=%s "
        "capabilities=0x00000005 command_availability=0x%08x "
        "active_role=%s active_version=v1 active_checksum=%s active_image_size=4096 "
        "running_partition=%u next_partition=%u boot_intent=%s "
        "%s "
        "partition_1_valid=1 partition_1_package_checksum=%s partition_1_package_size=3 "
        "partition_1_image_checksum=%s partition_1_image_size=2 partition_1_role=loader "
        "partition_1_version=v1 partition_1_board=devkit partition_1_target=esp32s3 "
        "partition_2_valid=1 partition_2_package_checksum=%s partition_2_package_size=3 "
        "partition_2_image_checksum=%s partition_2_image_size=2 partition_2_role=app "
        "partition_2_version=v1 partition_2_board=devkit partition_2_target=esp32s3 "
        "last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000\n",
        f->confirmation && f->confirmation->uid_from && f->confirmation->polls>=f->confirmation->uid_from?"112233445566":"102030405060",
        (unsigned)(0x0au | (f->confirmation?H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE:0u)),
        loader?"loader":"app",sha,loader?1u:2u,loader?1u:2u,loader?"loader":"auto",stage,sha,sha,sha,sha);
    assert(len>0 && (size_t)len<sizeof(line));
    if (f->mode==BAD_STATUS) strcpy(line,"H2_LOADER_STATUS invalid=1\n");
    if (f->mixed_status_output || f->status_crlf) {
        size_t end = strlen(line);
        assert(end + 1u < sizeof(line));
        line[end - 1u]='\r'; line[end]='\n'; line[end + 1u]='\0';
    }
    if (f->status_eol_test && f->status_crlf) f->status_prefix_split=strlen(line)-1u;
    if (f->status_prefix_split != 0u) {
        assert(f->status_prefix_split < strlen(line));
        assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)line,f->status_prefix_split)==H2_PAL_OK);
        assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
    }
    assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)line+f->status_prefix_split,
                               strlen(line)-f->status_prefix_split)==H2_PAL_OK);
}
static int peer_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    cli_fixture_t *f=user;
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
        if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN && f->pre_ack_log)
            assert(peer_output(f,f->pre_ack_log,strlen(f->pre_ack_log),&written,0u)==H2_PAL_OK);
        int rc=peer_output(f,encoded,len,&written,0u);
        if (rc==H2_PAL_OK && frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN && f->connect_ready_mode) {
            size_t prefix=f->connect_ready_mode==2?sizeof("H2_LOADER_REA")-1u:sizeof(connect_ready)-1u;
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)connect_ready,prefix)==H2_PAL_OK);
            assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
            const char *raw=f->connect_ready_mode==2?connect_ready+prefix:after_ready;
            assert(peer_output(f,raw,strlen(raw),&written,0u)==H2_PAL_OK);
            assert(written==strlen(raw));
        }
        if (rc==H2_PAL_OK && frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN && f->connect_output) {
            /* OPEN ACK, then decoded BOOT and raw CASE in the same UART read. */
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)mixed_boot,sizeof(mixed_boot)-1u)==H2_PAL_OK);
            assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
            assert(peer_output(f,mixed_raw,sizeof(mixed_raw)-1u,&written,0u)==H2_PAL_OK);
            assert(written==sizeof(mixed_raw)-1u);
        }
        return rc;
    }
    assert(frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_DATA && f->peer!=NULL);
    int rc=h2_iostreamikcp_input_frame(f->peer,frame);
    if (rc!=H2_PAL_OK) return rc;
    uint8_t command[64]; size_t len=0u;
    rc=h2_iostreamikcp_read(f->peer,command,sizeof(command),&len);
    if (rc==H2_PAL_OK && len!=0u) {
        const char status[]="h2loader status\n";
        const char reboot[]="h2loader reboot loader\n";
        if (len==sizeof(status)-1u && memcmp(command,status,len)==0) {
            ++f->requests;
            if (f->status_diagnostic != NULL)
                assert(h2_iostreamikcp_write(f->peer, (const uint8_t *)f->status_diagnostic,
                                           strlen(f->status_diagnostic)) == H2_PAL_OK);
            if (f->mixed_status_output && !f->connect_output) {
                size_t written = 0u;
                assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
                assert(peer_output(f,mixed_raw,sizeof(mixed_raw)-1u,&written,0u)==H2_PAL_OK);
                assert(written==sizeof(mixed_raw)-1u);
            }
            emit_status(f);
            if (f->status_eol_test) {
                const char *tail=f->status_eol_tail?f->status_eol_tail:mixed_boot;
                assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)tail,strlen(tail))==H2_PAL_OK);
            }
            if (f->mixed_status_output)
                assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)mixed_tail,sizeof(mixed_tail)-1u)==H2_PAL_OK);
        } else if (f->confirmation != NULL) {
            const char upgrade[]="h2loader reboot upgrade\n";
            const char accepted[]="H2_LOADER_REBOOT target=upgrade result=accepted\n";
            assert(len==sizeof(upgrade)-1u && memcmp(command,upgrade,len)==0);
            ++f->reboots;
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)accepted,sizeof(accepted)-1u)==H2_PAL_OK);
        } else {
            const char accepted[]="H2_LOADER_REBOOT target=loader result=accepted\n"
                "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\n";
            assert(len==sizeof(reboot)-1u && memcmp(command,reboot,len)==0);
            ++f->reboots;
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)accepted,sizeof(accepted)-1u)==H2_PAL_OK);
        }
    } else assert(rc==H2_PAL_ERR_WOULD_BLOCK || (rc==H2_PAL_OK && len==0u));
    return h2_iostreamikcp_flush(f->peer);
}
static int uart_write(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout; *written=len;
    return h2_iostreamikcp_filter_input(&f->filter,data,len,peer_frame,f);
}
static int uart_read(void *user, void *data, size_t cap, size_t *read, uint32_t timeout) {
    cli_fixture_t *f=user;
    /* Splitting already queued bytes does not consume a fresh I/O deadline. */
    f->now+=(f->mixed_status_output || f->status_eol_test) && f->input_offset<f->input_len?0u:timeout!=0u?timeout:1u;
    *read=f->input_len-f->input_offset;
    if (*read>cap) *read=cap;
    const size_t chunk=f->mixed_read_size!=0u?f->mixed_read_size:67u;
    if (*read>chunk) *read=chunk;
    if (*read==0u) return H2_PAL_ERR_TIMEOUT;
    memcpy(data,f->input+f->input_offset,*read); f->input_offset+=*read; return H2_PAL_OK;
}
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {.read=uart_read,.write=uart_write,.flush=flush};

static int scan(void *user, h2_pal_serial_host_snapshot_t **out) {
    cli_fixture_t *f=user; ++f->scans; *out=(h2_pal_serial_host_snapshot_t *)f; return H2_PAL_OK;
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
    cli_fixture_t *f=user; assert(*snapshot==(h2_pal_serial_host_snapshot_t *)f);
    ++f->snapshots_closed; *snapshot=NULL; return H2_PAL_OK;
}
static int open_port(void *user, const char *id, const h2_pal_uart_io_stream_config_t *config, h2_pal_serial_host_session_t **out) {
    cli_fixture_t *f=user; ++f->opens; assert(strcmp(id,"scan-port")==0 && config->baud_rate==460800u);
    assert(f->peer==NULL);
    h2_iostreamikcp_filter_init(&f->filter);
    if (f->mode==OPEN_FAILURE) return H2_PAL_ERR_BUSY;
    *out=(h2_pal_serial_host_session_t *)f; return H2_PAL_OK;
}
static int stream(void *user, h2_pal_serial_host_session_t *session, const h2_pal_uart_io_stream_api_t **out) {
    cli_fixture_t *f=user; assert(session==(h2_pal_serial_host_session_t *)f); ++f->streams;
    if (f->mode==STREAM_FAILURE) return H2_PAL_ERR_IO;
    *out=&f->uart; return H2_PAL_OK;
}
static int set_lines(void *user, h2_pal_serial_host_session_t *session, uint32_t mask, uint32_t lines) {
    cli_fixture_t *f=user; assert(session==(h2_pal_serial_host_session_t *)f); ++f->controls;
    f->modem_lines=(f->modem_lines&~mask)|(lines&mask); return H2_PAL_OK;
}
static int close_port(void *user, h2_pal_serial_host_session_t **session) {
    cli_fixture_t *f=user; assert(*session==(h2_pal_serial_host_session_t *)f);
    ++f->closes; *session=NULL;
    h2_iostreamikcp_close(f->peer); f->peer=NULL;
    f->input_len=f->input_offset=0u;
    return f->mode==CLOSE_FAILURE?H2_PAL_ERR_IO:H2_PAL_OK;
}
static const h2_pal_serial_host_vtable_t serial_vtable = {.scan=scan,.snapshot_count=count,
    .snapshot_get=port,.snapshot_destroy=snapshot_close,.open=open_port,.session_stream=stream,
    .set_control_lines=set_lines,.close=close_port};

static void run_case_with_output(int command, int mode, const char *diagnostic,
                                size_t mixed_read_size, size_t status_prefix_split, int connect_output) {
    cli_fixture_t f={.mode=mode,.modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
    f.mixed_status_output=mixed_read_size!=0u;
    f.mixed_read_size=mixed_read_size;
    f.status_prefix_split=status_prefix_split;
    f.connect_output=connect_output;
    f.status_diagnostic=connect_output?NULL:f.mixed_status_output?mixed_boot:diagnostic;
    f.cancel_after_opens=command==CLI_MONITOR?1u:command==CLI_REBOOT?2u:0u;
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_iostreamikcp_filter_init(&f.filter);
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
    h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
    const char *scan_argv[]={"h2loader","--no-ble","scan","--probe-timeout","1"};
    const char *status_argv[]={"h2loader","--port","scan-port","--wait-timeout","1","status"};
    const char *monitor_argv[]={"h2loader","--port","scan-port","monitor"};
    const char *reboot_argv[]={"h2loader","--port","scan-port","reboot","loader","--monitor"};
    const char *const *argv=command==CLI_SCAN?scan_argv:command==CLI_STATUS?status_argv:
        command==CLI_MONITOR?monitor_argv:reboot_argv;
    int argc=command==CLI_SCAN?5:command==CLI_MONITOR?4:6;
    h2_h2loader_cli_config_t config={.argc=argc,.argv=argv,.serial=&serial,.stdout_io=&io,.stderr_io=&io,
        .is_cancelled=cancelled,.cancel_user=&f};
    int result=h2_h2loader_cli_main(&runtime,&config);
    assert(result==(command==CLI_SCAN || mode==HEALTHY || mode==CLOSE_FAILURE || mode==WRONG_REBOOT_ROLE
        ?H2_H2LOADER_CLI_EXIT_OK:H2_H2LOADER_CLI_EXIT_RUNTIME));
    assert(f.scans==(command==CLI_SCAN?1u:0u) && f.snapshots_closed==f.scans);
    assert(f.opens==(command==CLI_REBOOT?2u:1u));
    assert(f.controls==0u);
    assert(f.modem_lines==(H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(f.closes==(mode==OPEN_FAILURE?0u:f.opens));
    if (command==CLI_SCAN) {
        assert(strstr(f.output,"\"port\": \"scan-port\"")!=NULL);
        if (mode==HEALTHY) {
            assert(f.requests==1u && strstr(f.output,"\"probe_result\": \"ok\"")!=NULL);
            assert(strstr(f.output,"\"device_uid\": \"102030405060\"")!=NULL);
        } else assert(strstr(f.output,"\"probe_result\": \"error\"")!=NULL);
    } else if (mode==HEALTHY || mode==CLOSE_FAILURE || mode==WRONG_REBOOT_ROLE) {
        assert(f.requests==(command==CLI_MONITOR?1u:2u));
    }
    if (command==CLI_REBOOT) {
        assert(f.reboots==1u && f.sleeps==1u);
        /* Correct role reaches monitor before closing; wrong role is rejected and closed before retry/cancel. */
        assert(f.cancel_closes==(mode==WRONG_REBOOT_ROLE?2u:1u));
    }
    assert(f.peer==NULL);
    assert(f.allocations==0u);
    if (connect_output && strcmp(f.output,diagnostic)!=0)
        fprintf(stderr,"connect pending expected=[%s] actual=[%s]\n",diagnostic,f.output);
    if (diagnostic != NULL) assert(strstr(f.output,diagnostic)!=NULL);
    if (diagnostic != NULL) assert(strstr(f.output,"H2_LOADER_STATUS ")==NULL);
    if (diagnostic != NULL) assert(f.output_len==strlen(diagnostic));
    if (connect_output && mixed_read_size>=512u)
        assert(f.before_status_output_len==sizeof(mixed_boot)+sizeof(mixed_raw)-2u);
}

static void run_case_with_diagnostic(int command, int mode, const char *diagnostic) {
    run_case_with_output(command,mode,diagnostic,0u,0u,0);
}

static void run_case(int command, int mode) {
    run_case_with_diagnostic(command, mode, NULL);
}

static void run_connect_ready(int mode, int sink) {
    cli_fixture_t f={.mode=HEALTHY,.mixed_read_size=512u,.connect_ready_mode=mode,
        .modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_h2loader_host_serial_connection_config_t config={.serial=&serial,.time=&time,.allocator=&f.mem,
        .port_id="scan-port",.preserve_control_lines=1,.on_log=sink?connect_log:NULL,.log_user=&f};
    h2_h2loader_host_serial_connection_t *connection=NULL;
    h2_pal_result_t rc=h2_h2loader_host_serial_connect(&config,&connection);
    if (sink) {
        if (rc!=H2_PAL_ERR_CLOSED)
            fprintf(stderr,"post-ACK READY mode=%d rc=%d output=[%s]\n",mode,rc,f.output);
        assert(rc==H2_PAL_ERR_CLOSED && connection==NULL);
        assert(f.output_len==sizeof(connect_ready)-1u && strcmp(f.output,connect_ready)==0);
    } else {
        assert(rc==H2_PAL_OK && connection!=NULL && f.output_len==0u);
        unsigned polls=1u;
        assert(h2_h2loader_host_serial_monitor_logs(connection,cancel_after_one_poll,&polls)==H2_PAL_ERR_CLOSED);
        assert(f.output_len==0u);
        assert(h2_h2loader_host_serial_disconnect(&connection)==H2_PAL_OK && connection==NULL);
    }
    assert(f.opens==1u && f.streams==1u && f.closes==1u && f.controls==0u);
    assert(f.requests==0u && f.reboots==0u && f.peer==NULL && f.allocations==0u);
    assert(f.modem_lines==(H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS));
}

static void run_status_boundary(const char *prefix, const char *tail, int crlf, size_t chunk,
                                int error_byte, const char *expected) {
    cli_fixture_t f={.pre_ack_log=prefix,.status_eol_tail=tail,.status_eol_test=1,.status_crlf=crlf,.mixed_read_size=chunk,
        .modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_h2loader_host_serial_connection_config_t config={.serial=&serial,.time=&time,.allocator=&f.mem,
        .port_id="scan-port",.preserve_control_lines=1,.on_log=connect_log,.log_user=&f};
    h2_h2loader_host_serial_connection_t *connection=NULL;
    h2_h2loader_host_status_t status;
    assert(h2_h2loader_host_serial_connect(&config,&connection)==H2_PAL_OK);
    f.log_error_byte=error_byte;
    h2_pal_result_t rc=h2_h2loader_host_serial_read_status(connection,&status);
    if (rc==H2_PAL_OK) {
        assert(status.running_partition==2u);
        unsigned polls=512u;
        rc=h2_h2loader_host_serial_monitor_logs(connection,cancel_after_one_poll,&polls);
    }
    assert(rc==(error_byte?H2_PAL_ERR_IO:H2_PAL_EXIT));
    assert(strcmp(f.output,expected)==0 && f.output_len==strlen(expected));
    assert(strstr(f.output,"H2_LOADER_STATUS ")==NULL);
    assert(h2_h2loader_host_serial_disconnect(&connection)==H2_PAL_OK && connection==NULL);
    assert(f.requests==1u && f.opens==1u && f.closes==1u && f.controls==0u);
    assert(f.peer==NULL && f.allocations==0u);
}

static void run_confirmation_cases(void) {
    static const struct {
        const char *name;
        unsigned confirm_at, cancel_at, bad_at, bad_from, uid_from, sleep_fail_at;
        int mismatched_stage, monitored, null_cancel, result;
        unsigned polls, sleeps, opens;
    } cases[]={
        {"already-confirmed",1,0,0,0,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_OK,1,1,2},
        {"pending150-then-stage0",151,0,0,0,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_OK,151,151,2},
        {"pending650-then-stage0",651,0,0,0,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_OK,651,651,2},
        {"pending-cancel",999,200,0,0,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_OK,200,200,2},
        {"pending-then120-status-errors",999,0,0,151,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_RUNTIME,270,270,121},
        {"pending-then120-UID-errors",999,0,0,0,151,0,0,1,0,H2_H2LOADER_CLI_EXIT_RUNTIME,270,270,121},
        {"pending-one-status-error-then-stage0",171,0,151,0,0,0,0,1,0,H2_H2LOADER_CLI_EXIT_OK,171,171,3},
        {"pending-sleep-error",999,0,0,0,0,151,0,1,0,H2_H2LOADER_CLI_EXIT_RUNTIME,150,151,2},
        {"plain-upgrade-bounded120",151,0,0,0,0,0,0,0,0,H2_H2LOADER_CLI_EXIT_RUNTIME,120,120,121},
        {"wrong-stage-bounded120",999,0,0,0,0,0,1,1,0,H2_H2LOADER_CLI_EXIT_RUNTIME,120,120,121},
        {"NULL-cancel-bounded120",151,0,0,0,0,0,0,1,1,H2_H2LOADER_CLI_EXIT_RUNTIME,120,120,121},
    };
    for (size_t i=0u;i<sizeof(cases)/sizeof(cases[0]);++i) {
        confirmation_probe_t p={.confirm_at=cases[i].confirm_at,.cancel_at=cases[i].cancel_at,
            .bad_at=cases[i].bad_at,.bad_from=cases[i].bad_from,.uid_from=cases[i].uid_from,
            .sleep_fail_at=cases[i].sleep_fail_at,.mismatched_stage=cases[i].mismatched_stage};
        cli_fixture_t f={.confirmation=&p,
            .modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
        f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
        f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
        h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
        h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
        h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
        h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
        const char *argv[]={"h2loader","--no-ble","--port","scan-port","reboot","upgrade","--monitor"};
        h2_h2loader_cli_config_t config={.argc=cases[i].monitored?7:6,.argv=argv,.serial=&serial,
            .stdout_io=&io,.stderr_io=&io,.is_cancelled=cases[i].null_cancel?NULL:cancelled,.cancel_user=&f};
        int result=h2_h2loader_cli_main(&runtime,&config);
        if (result!=cases[i].result || p.polls!=cases[i].polls || f.sleeps!=cases[i].sleeps || f.opens!=cases[i].opens)
            fprintf(stderr,"confirmation case=%s result=%d polls=%u sleeps=%u opens=%u\n",
                cases[i].name,result,p.polls,f.sleeps,f.opens);
        assert(result==cases[i].result && p.polls==cases[i].polls && f.sleeps==cases[i].sleeps);
        assert(f.opens==cases[i].opens && f.closes==f.opens && f.reboots==1u);
        assert(strstr(f.output,"H2_LOADER_REBOOT target=upgrade result=accepted\n")!=NULL);
        if (result==H2_H2LOADER_CLI_EXIT_OK && !cases[i].cancel_at) assert(!p.stage_valid);
        else assert(p.stage_valid);
        if (cases[i].cancel_at) assert(p.cancel_seen);
        assert(f.controls==0u && f.modem_lines==(H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS));
        assert(f.peer==NULL && f.allocations==0u);
    }
}

int main(void) {
    for (int mode=HEALTHY; mode<=CLOSE_FAILURE; ++mode) {
        run_case(CLI_SCAN,mode);
        run_case(CLI_STATUS,mode);
    }
    run_case(CLI_MONITOR,HEALTHY);
    run_case(CLI_REBOOT,HEALTHY);
    run_case(CLI_REBOOT,WRONG_REBOOT_ROLE);
    run_case_with_diagnostic(CLI_MONITOR, HEALTHY, "H2_TEST_STATUS_DIAGNOSTIC received\n");
    const size_t chunks[]={1024u,67u,1u};
    for (size_t chunk=0u;chunk<sizeof(chunks)/sizeof(chunks[0]);++chunk)
        for (size_t split=0u;split<sizeof("H2_LOADER_STATUS ")-1u;++split)
            run_case_with_output(CLI_MONITOR,HEALTHY,mixed_expected,chunks[chunk],split,0);
    for (size_t chunk=0u;chunk<sizeof(chunks)/sizeof(chunks[0]);++chunk)
        run_case_with_output(CLI_MONITOR,HEALTHY,mixed_expected,chunks[chunk],0u,1);
    run_connect_ready(1,1);
    run_connect_ready(2,1);
    run_connect_ready(1,0);
    run_status_boundary(">de",NULL,0,512u,0,">deH2_DISPLAY_BOOT version=test\r\n");
    run_status_boundary(">de\r\n",NULL,0,512u,0,">de\r\nH2_DISPLAY_BOOT version=test\r\n");
    run_status_boundary(NULL,NULL,0,512u,0,mixed_boot);
    run_status_boundary(">de",NULL,1,1u,0,">deH2_DISPLAY_BOOT version=test\r\n");
    run_status_boundary(">de\r\n",NULL,1,1u,0,">de\r\nH2_DISPLAY_BOOT version=test\r\n");
    run_status_boundary(NULL,NULL,1,1u,0,mixed_boot);
    run_status_boundary(">de",NULL,0,512u,'\n',">deH2_DISPLAY_BOOT version=test\r");
    run_status_boundary(">de",NULL,1,1u,'\r',">deH2_DISPLAY_BOOT version=test");
    run_status_boundary(">de",NULL,1,1u,'\n',">deH2_DISPLAY_BOOT version=test\r");
    const char link_tail[]="stage=start role=join state=joining\r\n";
    for (int crlf=0;crlf<2;++crlf) {
        for (size_t chunk=0u;chunk<sizeof(chunks)/sizeof(chunks[0]);++chunk) {
            run_status_boundary("LINK ",link_tail,crlf,chunks[chunk],0,
                "LINK stage=start role=join state=joining\r\n");
            run_status_boundary("LINK \r\n",link_tail,crlf,chunks[chunk],0,
                "LINK \r\nstage=start role=join state=joining\r\n");
        }
    }
    run_confirmation_cases();
    return 0;
}
