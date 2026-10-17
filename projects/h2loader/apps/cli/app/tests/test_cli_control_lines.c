#include "h2_h2loader_cli_app.h"
#include "h2_iostreamikcp.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEALTHY, OPEN_FAILURE, STREAM_FAILURE, HANDSHAKE_TIMEOUT, BAD_STATUS, CLOSE_FAILURE, WRONG_REBOOT_ROLE,
    OLD_WIFI_TARGET, WIFI_ERROR, WIFI_LEGACY_REPLY, WIFI_HANDSHAKE_LOG_ERROR,
    WIFI_RUNNING_LOG_ERROR, WIFI_MONITOR_LOG_ERROR, WIFI_MONITOR_READ_ERROR,
    REBOOT_PENDING, REBOOT_BAD_UID, REBOOT_BAD_IMAGE, REBOOT_BAD_P1, REBOOT_BAD_FINAL,
    REBOOT_DISCONNECT, REBOOT_POST_DISCONNECT, REBOOT_FOREVER, REBOOT_LONG,
    REBOOT_CANCEL, REBOOT_CLOCK_ERROR, REBOOT_LOG_ERROR, REBOOT_NORMAL_BAD_IMAGE,
    REBOOT_LATE_HANDSHAKE, REBOOT_LATE_RESPONSE, REBOOT_CANCEL_QUERY };
enum { CLI_SCAN, CLI_STATUS, CLI_MONITOR, CLI_REBOOT };
typedef struct cli_fixture {
    int mode;
    int delayed_status, live_fd, response_pending, delayed_prefix_sent;
    unsigned delayed_requests, request_at_delay, device_logs, gap_logs;
    uint64_t delayed_started, response_due, event_due;
    int fail_write_result, fail_clock_result, fail_log_result;
    int raw_log_error, raw_log_emitted;
    uint64_t raw_log_due;
    unsigned opens, streams, closes, controls, scans, snapshots_closed, requests;
    unsigned reboots, sleeps, cancel_after_opens, cancel_closes;
    unsigned wifi_connects, wifi_temporary;
    int trace_wifi, wifi_monitor_requested, password_is_monitor;
    unsigned ready_log_seen, open_log_seen, running_log_seen, tunnel_log_seen;
    unsigned wifi_terminal_seen, terminal_cancel_checks, monitor_started, monitor_log_seen;
    int monitor_read_result;
    int trace_reboot, stage_cleared, physical_down;
    unsigned native_step, boot_emitted, post_status_count, final_status_count;
    unsigned did_disconnect, reboot_log_failed;
    unsigned status_emitted;
    int drop_delivery, drop_response, delay_write, status_cr_only;
    uint64_t io_deadline;
    uint32_t largest_write_timeout;
    size_t read_limit;
    uint64_t boot_ms, native_due;
    uint32_t modem_lines;
    uint64_t now;
    size_t allocations;
    uint8_t input[16384];
    size_t input_len, input_offset;
    char output[16384];
    size_t output_len;
    char error_output[1024];
    size_t error_output_len;
    h2_iostreamikcp_t *peer;
    h2_iostreamikcp_filter_t filter;
    h2_pal_mem_api_t mem;
    h2_pal_uart_io_stream_api_t uart;
} cli_fixture_t;

static void queue_raw_log(cli_fixture_t *f, const char *line);
static void queue_monitor_log(cli_fixture_t *f);
static void delayed_device_tick(cli_fixture_t *f);

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
static int clock_ms(void *user, uint64_t *out) {
    cli_fixture_t *f=user;
    if (f->fail_clock_result && f->requests) return f->fail_clock_result;
    if (f->mode==REBOOT_CLOCK_ERROR && f->native_step>=3u) return H2_PAL_ERR_IO;
    *out=f->now; return H2_PAL_OK;
}
static uint32_t peer_ms(void *user) { return (uint32_t)((cli_fixture_t *)user)->now; }
static int sleep_ms(void *user, uint32_t duration) {
    cli_fixture_t *f=user; ++f->sleeps; f->now+=duration; delayed_device_tick(f); return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms=clock_ms,.sleep_ms=sleep_ms};
static int cancelled(void *user) {
    cli_fixture_t *f=user;
    if (f->delayed_status) {
        if (f->delayed_status==6 && f->delayed_started && f->now>=f->delayed_started+300u) return 1;
        return f->device_logs && f->final_status_count;
    }
    if (f->trace_reboot) {
        if ((f->mode==REBOOT_CANCEL || f->mode==REBOOT_CANCEL_QUERY) && f->native_step>=3u) return 1;
        if (f->mode==REBOOT_LONG && f->now-f->boot_ms<130000u) return 0;
        return f->native_step==10u && f->final_status_count!=0u &&
            (f->mode!=REBOOT_POST_DISCONNECT ||
             (f->did_disconnect && f->opens==3u && f->final_status_count>=2u));
    }
    if (f->wifi_monitor_requested && f->wifi_terminal_seen) {
        /* Host Core checks cancellation after reading the terminal; the next
         * check starts monitor_logs after its response-ACK grace completes. */
        if (++f->terminal_cancel_checks==2u) {
            f->monitor_started=1u;
            if (f->mode==WIFI_MONITOR_READ_ERROR) f->monitor_read_result=H2_PAL_ERR_CLOSED;
            else queue_monitor_log(f);
        }
        if (f->monitor_log_seen) return 1;
    }
    if (f->cancel_after_opens!=0u && f->opens>=f->cancel_after_opens) {
        f->cancel_closes=f->closes; return 1;
    }
    return 0;
}
static int output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout;
    assert(len < sizeof(f->output)-f->output_len);
    memcpy(f->output+f->output_len,data,len); f->output_len+=len;
    f->output[f->output_len]='\0'; *written=len;
    if (f->fail_log_result && strstr(f->output,f->raw_log_error
        ? "RAW_LOG_FAILURE\n" : "H2_LOADER_STATU_note actual-console\n")!=NULL) {
        return f->fail_log_result;
    }
    if (f->mode==REBOOT_LOG_ERROR && !f->reboot_log_failed &&
        strstr(f->output,"REBOOT_NATIVE_CASE id=2\n")!=NULL) {
        f->reboot_log_failed=1u;
        return H2_PAL_ERR_TIMEOUT;
    }
    if (f->trace_wifi) {
        if (!f->ready_log_seen && strstr(f->output,"WIFI_READY\n")!=NULL) {
            assert(f->peer==NULL && f->wifi_connects+f->wifi_temporary==0u);
            f->ready_log_seen=1u;
            if (f->mode==WIFI_HANDSHAKE_LOG_ERROR) return H2_PAL_ERR_IO;
        }
        if (!f->open_log_seen && strstr(f->output,"WIFI_OPEN_LOG\n")!=NULL) {
            assert(f->wifi_connects+f->wifi_temporary==0u);
            f->open_log_seen=1u;
        }
        if (!f->running_log_seen && strstr(f->output,"WIFI_DURING_LOG\n")!=NULL) {
            assert(f->wifi_connects+f->wifi_temporary==1u && !f->wifi_terminal_seen);
            f->running_log_seen=1u;
            if (f->mode==WIFI_RUNNING_LOG_ERROR) return H2_PAL_ERR_IO;
        }
        if (strstr(f->output,"WIFI_TUNNEL_LOG\n")!=NULL) f->tunnel_log_seen=1u;
        if (strstr(f->output,"result=connected ssid=factory\n")!=NULL) f->wifi_terminal_seen=1u;
        if (!f->monitor_log_seen && strstr(f->output,"WIFI_MONITOR_LOG\n")!=NULL) {
            assert(f->monitor_started && f->wifi_terminal_seen);
            f->monitor_log_seen=1u;
            if (f->mode==WIFI_MONITOR_LOG_ERROR) return H2_PAL_ERR_IO;
        }
    }
    return H2_PAL_OK;
}
static int flush(void *user) { (void)user; return H2_PAL_OK; }
static const h2_command_io_vtable_t output_vtable = {.write=output, .flush=flush};
static int error_output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout;
    assert(len < sizeof(f->error_output)-f->error_output_len);
    memcpy(f->error_output+f->error_output_len,data,len); f->error_output_len+=len;
    f->error_output[f->error_output_len]='\0'; *written=len; return H2_PAL_OK;
}
static const h2_command_io_vtable_t error_output_vtable = {.write=error_output, .flush=flush};

static int peer_output(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout;
    if (f->input_offset == f->input_len) f->input_offset=f->input_len=0u;
    assert(len <= sizeof(f->input)-f->input_len);
    memcpy(f->input+f->input_len,data,len); f->input_len+=len;
    *written=len; return H2_PAL_OK;
}
static void queue_raw_log(cli_fixture_t *f, const char *line) {
    size_t written=0u;
    assert(peer_output(f,line,strlen(line),&written,0u)==H2_PAL_OK);
}
static void queue_monitor_log(cli_fixture_t *f) {
    const char line[]="WIFI_MONITOR_LOG\n";
    assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)line,sizeof(line)-1u)==H2_PAL_OK);
    assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
}

static void queue_preack_data(cli_fixture_t *f, uint32_t conv) {
    static const char text[]="PRE_ACK_DATA_UNOBSERVED\n";
    uint8_t payload[24u+sizeof(text)-1u]={0};
    for (unsigned i=0u;i<4u;++i) payload[i]=(uint8_t)(conv>>(8u*i));
    payload[4]=81u;payload[6]=64u;payload[20]=sizeof(text)-1u;
    memcpy(payload+24u,text,sizeof(text)-1u);
    const h2_iostreamikcp_frame_t frame={.flags=H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
        .conv=conv,.payload=payload,.payload_len=sizeof(payload)};
    uint8_t encoded[128];size_t length=0u,written=0u;
    assert(h2_iostreamikcp_frame_encode(&frame,encoded,sizeof(encoded),&length)==H2_PAL_OK);
    assert(peer_output(f,encoded,length,&written,0u)==H2_PAL_OK);
}

static void replace_status_field(char *line, size_t capacity, const char *key, const char *value) {
    char field[64];
    assert(snprintf(field,sizeof(field),"%s=",key)>0);
    char *start=strstr(line,field);assert(start!=NULL);start+=strlen(field);
    char *end=start;while(*end!='\0' && *end!=' ' && *end!='\n') ++end;
    size_t n=strlen(value),old=(size_t)(end-start);
    assert(strlen(line)-old+n<capacity);
    memmove(start+n,end,strlen(end)+1u);memcpy(start,value,n);
}

static void reboot_status(cli_fixture_t *f, char *line, size_t capacity) {
    static const char candidate[]="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    static const char wrong[]="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    const int post=f->reboots!=0u;
    const int stage=f->mode!=REBOOT_NORMAL_BAD_IMAGE && (!post || !f->stage_cleared);
#define FIELD(key,value) replace_status_field(line,capacity,(key),(value))
    FIELD("command_availability","0x002a000b");
    if (stage) {
        FIELD("stage_valid","1");FIELD("stage_package_checksum",candidate);
        FIELD("stage_package_size","3");FIELD("stage_image_checksum",candidate);
        FIELD("stage_image_size","4096");FIELD("stage_role","app");
        FIELD("stage_version","v2");FIELD("stage_board","devkit");FIELD("stage_target","esp32s3");
    }
    if (post) {
        ++f->post_status_count;
        FIELD("active_role","app");FIELD("active_version","v2");
        FIELD("active_checksum",f->mode==REBOOT_BAD_IMAGE?wrong:candidate);
        FIELD("running_partition","2");FIELD("next_partition","2");FIELD("boot_intent","auto");
        if (f->mode==REBOOT_BAD_UID) FIELD("device_uid","aabbccddeeff");
        if (f->mode==REBOOT_BAD_P1) FIELD("partition_1_image_checksum",wrong);
        if (!stage) {
            ++f->final_status_count;
            FIELD("partition_2_package_checksum",f->mode==REBOOT_BAD_FINAL?wrong:candidate);
            FIELD("partition_2_image_checksum",candidate);FIELD("partition_2_version","v2");
        }
    }
#undef FIELD
}
static void emit_status(cli_fixture_t *f) {
    ++f->status_emitted;
    const char *sha="abababababababababababababababababababababababababababababababab";
    int loader=f->reboots!=0u && f->mode!=WRONG_REBOOT_ROLE;
    char line[4096];
    int len=snprintf(line,sizeof(line),
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 device_uid=102030405060 "
        "capabilities=0x00000007 command_availability=0x%08x "
        "active_role=%s active_version=v1 active_checksum=%s active_image_size=4096 "
        "running_partition=%u next_partition=%u boot_intent=%s "
        "stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- "
        "stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=- "
        "partition_1_valid=1 partition_1_package_checksum=%s partition_1_package_size=3 "
        "partition_1_image_checksum=%s partition_1_image_size=4096 partition_1_role=loader "
        "partition_1_version=v1 partition_1_board=devkit partition_1_target=esp32s3 "
        "partition_2_valid=1 partition_2_package_checksum=%s partition_2_package_size=3 "
        "partition_2_image_checksum=%s partition_2_image_size=4096 partition_2_role=app "
        "partition_2_version=v1 partition_2_board=devkit partition_2_target=esp32s3 "
        "last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000\n",
        f->mode==OLD_WIFI_TARGET?0x0002000au:0x0022000au,
        loader?"loader":"app",sha,loader?1u:2u,loader?1u:2u,loader?"loader":"auto",sha,sha,sha,sha);
    assert(len>0 && (size_t)len<sizeof(line));
    if (f->trace_reboot) {
        const char log[]="H2_LOADER_STATU_note actual-console\n";
        reboot_status(f,line,sizeof(line));
        if (f->status_cr_only!=2 && !f->delayed_prefix_sent) {
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)log,sizeof(log)-1u)==H2_PAL_OK);
        }
    }
    if (f->mode==BAD_STATUS) strcpy(line,"H2_LOADER_STATUS invalid=1\n");
    if (f->status_cr_only) {
        const char tail[]="STATUS_TAIL_ACTUAL_CASE\n";
        if (f->status_cr_only==1) line[strlen(line)-1u]='\r';
        assert(strlen(line)+sizeof(tail)<=sizeof(line));
        strcat(line,tail);
    }
    if (f->status_cr_only==2) {
        /* One KCP write places the marker at byte 1017: its prefix must
         * survive the Host response reader's 1024-byte decoded boundary. */
        static const char log[]="H2_LOADER_STATU_note actual-console\n";
        const size_t prefix=1017u;
        assert(prefix+strlen(line)<sizeof(line));
        memmove(line+prefix,line,strlen(line)+1u);
        memset(line,'x',prefix);
        memcpy(line,log,sizeof(log)-1u);
        line[prefix-1u]='\n';
    }
    const char *wire_line=line+(f->delayed_prefix_sent?strlen("H2_LOADER_STA"):0u);
    assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)wire_line,strlen(wire_line))==H2_PAL_OK);
    f->delayed_prefix_sent=0;
}

static void delayed_device_tick(cli_fixture_t *f) {
    if (!f->delayed_status || !f->delayed_started || f->delayed_status==4) return;
    if (f->delayed_status==5 && !f->did_disconnect && f->now>=f->delayed_started+700u) {
        f->did_disconnect=1u;f->physical_down=1;
    }
    if ((f->delayed_status==3 || f->delayed_status==7) && !f->delayed_prefix_sent &&
        f->now>=f->delayed_started+850u) {
        const char prefix[]="H2_LOADER_STATU_note actual-console\nH2_LOADER_STA";
        assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)prefix,sizeof(prefix)-1u)==H2_PAL_OK);
        assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
        f->delayed_prefix_sent=1;
    }
    if (f->delayed_status==7) return;
    if (!f->device_logs && f->now>=f->event_due) {
        ++f->device_logs;f->stage_cleared=1;f->native_step=10u;
        const char *line="LIBCO_CONFIRM rc=0\nLIBCO_AFTER_CONFIRM rc=0\nLIBCO_READY rc=0\n";
        if (f->live_fd) queue_raw_log(f,line); else ++f->gap_logs;
    }
    if (f->response_pending && f->now>=f->response_due) {
        f->response_pending=0;
        if (f->live_fd && f->peer) {
            emit_status(f);
            assert(h2_iostreamikcp_flush(f->peer)==H2_PAL_OK);
        }
    }
}
static int peer_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    cli_fixture_t *f=user;
    if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN ||
        frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE) {
        if (f->mode==HANDSHAKE_TIMEOUT) return H2_PAL_OK;
        if (frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
            if (f->mode==REBOOT_LATE_HANDSHAKE && f->did_disconnect) return H2_PAL_OK;
            assert(f->peer==NULL);
            h2_iostreamikcp_config_t config={.io={.user=f,.write=peer_output},.allocator=&f->mem,
                .conv=frame->conv,.now_ms=peer_ms,.time_user=f,.rx_buffer_size=4096u};
            assert(h2_iostreamikcp_open(&config,&f->peer)==H2_PAL_OK);
            if (f->trace_wifi) queue_raw_log(f,"WIFI_OPEN_LOG\n");
            if (f->trace_reboot && f->reboots && !f->boot_emitted) {
                f->boot_emitted=1u;f->boot_ms=f->now;f->native_due=f->now+100u;
                queue_raw_log(f,"REBOOT_NATIVE_BOOT\n");
                /* Existing pre-ACK DATA handling is a separate boundary:
                 * unlike native bytes, its payload is not a console event. */
                queue_preack_data(f,frame->conv);
            }
        }
        h2_iostreamikcp_frame_t ack=*frame; ack.flags=H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK;
        uint8_t encoded[32]; size_t len=0u, written=0u;
        assert(h2_iostreamikcp_frame_encode(&ack,encoded,sizeof(encoded),&len)==H2_PAL_OK);
        int rc=peer_output(f,encoded,len,&written,0u);
        if (f->trace_reboot && frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
            queue_raw_log(f,"REBOOT_POST_ACK_NATIVE\n");
        }
        return rc;
    }
    assert(frame->flags==H2_IOSTREAMIKCP_FRAME_FLAG_DATA && f->peer!=NULL);
    if (f->drop_delivery) return H2_PAL_OK;
    int rc=h2_iostreamikcp_input_frame(f->peer,frame);
    if (rc!=H2_PAL_OK) return rc;
    uint8_t command[160]; size_t len=0u;
    rc=h2_iostreamikcp_read(f->peer,command,sizeof(command),&len);
    if (rc==H2_PAL_OK && len!=0u) {
        const char status[]="h2loader status\n";
        const char *reboot=f->mode==REBOOT_NORMAL_BAD_IMAGE?"h2loader reboot app\n"
            : f->trace_reboot?"h2loader reboot upgrade\n":"h2loader reboot loader\n";
        if (len==sizeof(status)-1u && memcmp(command,status,len)==0) {
            ++f->requests;
            if (f->raw_log_error) f->raw_log_due=f->now+50u;
            if (f->delayed_status && f->boot_emitted && !f->device_logs && !f->delayed_started &&
                ++f->delayed_requests==(f->delayed_status==2?2u:1u)) {
                f->response_pending=1;f->delayed_started=f->now;
                f->request_at_delay=f->requests;
                if (f->delayed_status==3 || f->delayed_status==7) f->status_cr_only=1;
                f->event_due=f->now+1250u;f->response_due=f->event_due;
            } else if (f->delayed_status==5 && f->did_disconnect && !f->device_logs) {
                f->response_pending=1;
            } else if (!f->drop_response && !(f->mode==REBOOT_CANCEL_QUERY && f->boot_emitted) &&
                !(f->mode==REBOOT_LATE_RESPONSE &&
                f->boot_emitted && f->now-f->boot_ms>=118000u)) emit_status(f);
        } else if (len>=14u && memcmp(command,"h2loader wifi ",14u)==0) {
            const char *saved=f->password_is_monitor
                ? "h2loader wifi connect factory --monitor\n" : "h2loader wifi connect factory secret\n";
            const char *temporary=f->password_is_monitor
                ? "h2loader wifi connect-temporary factory --monitor\n" : "h2loader wifi connect-temporary factory secret\n";
            const int is_temporary=len==strlen(temporary) && memcmp(command,temporary,len)==0;
            assert(is_temporary || (len==strlen(saved) && memcmp(command,saved,len)==0));
            if (is_temporary) ++f->wifi_temporary;
            else ++f->wifi_connects;
            if (f->trace_wifi) {
                const char tunneled[]="WIFI_TUNNEL_LOG\n";
                queue_raw_log(f,"WIFI_DURING_LOG\n");
                assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)tunneled,sizeof(tunneled)-1u)==H2_PAL_OK);
            }
            const char *response=is_temporary && f->mode!=WIFI_LEGACY_REPLY
                ? f->mode==WIFI_ERROR ? "H2_LOADER_WIFI_TEMPORARY result=error code=-6\n"
                    : "H2_LOADER_WIFI_TEMPORARY result=connected ssid=factory\n"
                : "H2_LOADER_WIFI result=connected ssid=factory\n";
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)response,strlen(response))==H2_PAL_OK);
        } else {
            const char *accepted=f->mode==REBOOT_NORMAL_BAD_IMAGE
                ? "H2_LOADER_REBOOT target=app result=accepted\nH2_LOADER_REBOOT_FINAL target=app result=OK code=0\n"
                : f->trace_reboot
                ? "H2_LOADER_REBOOT target=upgrade result=accepted\nH2_LOADER_REBOOT_FINAL target=upgrade result=OK code=0\n"
                : "H2_LOADER_REBOOT target=loader result=accepted\nH2_LOADER_REBOOT_FINAL target=loader result=OK code=0\n";
            assert(len==strlen(reboot) && memcmp(command,reboot,len)==0);
            ++f->reboots;
            assert(h2_iostreamikcp_write(f->peer,(const uint8_t *)accepted,strlen(accepted))==H2_PAL_OK);
        }
    } else assert(rc==H2_PAL_ERR_WOULD_BLOCK || (rc==H2_PAL_OK && len==0u));
    return h2_iostreamikcp_flush(f->peer);
}
static int uart_write(void *user, const void *data, size_t len, size_t *written, uint32_t timeout) {
    cli_fixture_t *f=user; (void)timeout; *written=len;
    if (f->fail_write_result) { *written=0u;return f->fail_write_result; }
    if (timeout>f->largest_write_timeout) f->largest_write_timeout=timeout;
    if (f->delay_write) {
        assert(f->now<f->io_deadline && timeout<=f->io_deadline-f->now);
        f->now+=timeout;*written=0u;return H2_PAL_ERR_TIMEOUT;
    }
    return h2_iostreamikcp_filter_input(&f->filter,data,len,peer_frame,f);
}
static int uart_read(void *user, void *data, size_t cap, size_t *read, uint32_t timeout) {
    cli_fixture_t *f=user; f->now+=timeout!=0u?timeout:1u;delayed_device_tick(f);
    if (f->raw_log_error && !f->raw_log_emitted && f->raw_log_due &&
        f->now>=f->raw_log_due && f->input_offset==f->input_len) {
        f->raw_log_emitted=1;
        queue_raw_log(f,"RAW_LOG_FAILURE\n");
    }
    if (f->delayed_status && f->physical_down) { *read=0u;return H2_PAL_ERR_CLOSED; }
    if (f->trace_reboot && f->boot_emitted && !f->delayed_status) {
        const int disconnect=(f->mode==REBOOT_DISCONNECT && f->native_step>=3u) ||
            (f->mode==REBOOT_POST_DISCONNECT && f->final_status_count!=0u) ||
            (f->mode==REBOOT_LATE_HANDSHAKE && f->now-f->boot_ms>=118000u);
        if (!f->did_disconnect && disconnect && f->input_offset==f->input_len) {
            f->did_disconnect=1u;f->physical_down=1;
        }
        if (f->physical_down) { *read=0u;return H2_PAL_ERR_CLOSED; }
        if (f->mode==REBOOT_LONG && f->now-f->boot_ms>=90000u) f->stage_cleared=1;
        if (f->input_offset==f->input_len && f->now>=f->native_due && f->native_step<10u) {
            char line[64];
            if (f->native_step<8u) snprintf(line,sizeof(line),"REBOOT_NATIVE_CASE id=%u\n",f->native_step);
            else if (f->native_step==8u) {
                strcpy(line,"REBOOT_NATIVE_REPORT cases=8\n");
                if (f->mode!=REBOOT_LONG && f->mode!=REBOOT_FOREVER &&
                    f->mode!=REBOOT_LATE_HANDSHAKE && f->mode!=REBOOT_LATE_RESPONSE) f->stage_cleared=1;
            } else {
                if (!f->stage_cleared) { *read=0u;return H2_PAL_ERR_TIMEOUT; }
                strcpy(line,"REBOOT_NATIVE_READY\n");
            }
            ++f->native_step;f->native_due=f->now+100u;queue_raw_log(f,line);
        }
    }
    if (f->monitor_read_result!=H2_PAL_OK) { *read=0u; return f->monitor_read_result; }
    *read=f->input_len-f->input_offset;
    if (*read>cap) *read=cap;
    size_t limit=f->read_limit?f->read_limit:67u;
    if (*read>limit) *read=limit;
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
    f->physical_down=0;
    f->live_fd=1;
    if (f->mode==OPEN_FAILURE) return H2_PAL_ERR_BUSY;
    if (f->trace_wifi) queue_raw_log(f,"WIFI_READY\n");
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
    f->live_fd=0;
    if (f->delayed_status==5) f->response_pending=0;
    h2_iostreamikcp_close(f->peer); f->peer=NULL;
    f->input_len=f->input_offset=0u;
    return f->mode==CLOSE_FAILURE?H2_PAL_ERR_IO:H2_PAL_OK;
}
static const h2_pal_serial_host_vtable_t serial_vtable = {.scan=scan,.snapshot_count=count,
    .snapshot_get=port,.snapshot_destroy=snapshot_close,.open=open_port,.session_stream=stream,
    .set_control_lines=set_lines,.close=close_port};

static void run_case(int command, int mode) {
    cli_fixture_t f={.mode=mode,.modem_lines=H2_PAL_SERIAL_HOST_CONTROL_DTR|H2_PAL_SERIAL_HOST_CONTROL_RTS};
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
        /* A healthy old role is pending, and retains this fd until cancellation. */
        assert(f.cancel_closes==1u);
    }
    assert(f.peer==NULL);
    assert(f.allocations==0u);
}

static void run_wifi_case(int temporary, int mode) {
    cli_fixture_t f={.mode=mode};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_iostreamikcp_filter_init(&f.filter);
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
    h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
    const char *argv[]={"h2loader","--port","scan-port","--read-timeout","1",
        "wifi","connect","factory","secret","--temporary","--temporary"};
    const int argc=temporary==2?11:temporary?10:9;
    h2_h2loader_cli_config_t config={.argc=argc,.argv=argv,.serial=&serial,.stdout_io=&io,.stderr_io=&io};
    int result=h2_h2loader_cli_main(&runtime,&config);
    if (temporary==2) {
        assert(result==H2_H2LOADER_CLI_EXIT_USAGE);
        assert(f.opens==0u && f.wifi_connects==0u && f.wifi_temporary==0u);
    } else {
        assert(result==(mode==HEALTHY?H2_H2LOADER_CLI_EXIT_OK:H2_H2LOADER_CLI_EXIT_RUNTIME));
        assert(f.opens==1u && f.closes==1u);
        assert(f.wifi_connects==(temporary?0u:1u));
        assert(f.wifi_temporary==(temporary && mode!=OLD_WIFI_TARGET?1u:0u));
    }
    assert(strstr(f.output,"secret")==NULL);
    assert(f.peer==NULL && f.allocations==0u);
}

static void run_wifi_monitor_case(int temporary, int monitor, int mode, int password_is_monitor) {
    cli_fixture_t f={.mode=mode,.trace_wifi=1,.wifi_monitor_requested=monitor,.password_is_monitor=password_is_monitor};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_iostreamikcp_filter_init(&f.filter);
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
    h2_command_io_api_t error_io={.user=&f,.vtable=&error_output_vtable};
    h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
    const char *argv[]={"h2loader","--port","scan-port","--ready","WIFI_READY",
        "wifi","connect","factory","secret",NULL,NULL};
    if (password_is_monitor) argv[8]="--monitor";
    int argc=9;
    if (temporary) argv[argc++]="--temporary";
    if (monitor) argv[argc++]="--monitor";
    h2_h2loader_cli_config_t config={.argc=argc,.argv=argv,.serial=&serial,.stdout_io=&io,.stderr_io=&error_io,
        .is_cancelled=cancelled,.cancel_user=&f};
    int result=h2_h2loader_cli_main(&runtime,&config);
    assert(result==(mode==HEALTHY?H2_H2LOADER_CLI_EXIT_OK:H2_H2LOADER_CLI_EXIT_RUNTIME));
    assert(f.opens==1u && f.closes==1u && f.reboots==0u);
    unsigned expected_wifi=mode==WIFI_HANDSHAKE_LOG_ERROR || mode==OLD_WIFI_TARGET?0u:1u;
    assert(f.wifi_connects==(temporary?0u:expected_wifi));
    assert(f.wifi_temporary==(temporary?expected_wifi:0u));
    assert(f.requests==(mode==WIFI_HANDSHAKE_LOG_ERROR?0u:1u));
    if (monitor) {
        assert(f.ready_log_seen==1u);
        if (mode!=WIFI_HANDSHAKE_LOG_ERROR) assert(f.open_log_seen==1u);
        if (expected_wifi) assert(f.running_log_seen==1u);
        if (mode==HEALTHY || mode==CLOSE_FAILURE || mode==WIFI_MONITOR_LOG_ERROR || mode==WIFI_MONITOR_READ_ERROR) {
            assert(f.tunnel_log_seen==1u && f.monitor_started==1u);
            assert(f.monitor_log_seen==(mode==WIFI_MONITOR_READ_ERROR?0u:1u));
        } else assert(f.monitor_started==0u);
    } else {
        assert(f.ready_log_seen==0u && f.open_log_seen==0u && f.running_log_seen==0u);
        assert(f.tunnel_log_seen==1u && f.monitor_started==0u);
    }
    if (mode==WIFI_HANDSHAKE_LOG_ERROR || mode==WIFI_RUNNING_LOG_ERROR || mode==WIFI_MONITOR_LOG_ERROR || mode==CLOSE_FAILURE) {
        assert(strstr(f.error_output,"command failed code=-4")!=NULL);
    }
    if (mode==WIFI_MONITOR_READ_ERROR) assert(strstr(f.error_output,"command failed code=-10")!=NULL);
    assert(strstr(f.output,"secret")==NULL);
    assert(strstr(f.error_output,"secret")==NULL);
    if (password_is_monitor) {
        assert(strstr(f.output,"--monitor")==NULL);
        assert(strstr(f.error_output,"--monitor")==NULL);
    }
    assert(f.peer==NULL && f.allocations==0u);
}

static unsigned occurrences(const char *text, const char *needle) {
    unsigned count=0u;while((text=strstr(text,needle))!=NULL) {++count;text+=strlen(needle);}return count;
}

static void run_reboot_capture_case(int mode) {
    cli_fixture_t f={.mode=mode,.trace_reboot=1};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_iostreamikcp_filter_init(&f.filter);
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
    h2_command_io_api_t error_io={.user=&f,.vtable=&error_output_vtable};
    h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
    const char *argv[]={"h2loader","--port","scan-port","--wait-timeout",
        mode==REBOOT_FOREVER?"65":"120","--read-timeout",mode==REBOOT_CANCEL_QUERY?"1":"120","reboot",
        mode==REBOOT_NORMAL_BAD_IMAGE?"app":"upgrade","--monitor"};
    h2_h2loader_cli_config_t config={.argc=10,.argv=argv,.serial=&serial,.stdout_io=&io,.stderr_io=&error_io,
        .is_cancelled=cancelled,.cancel_user=&f};
    int result=h2_h2loader_cli_main(&runtime,&config);
    const int success=mode==REBOOT_PENDING || mode==REBOOT_DISCONNECT ||
        mode==REBOOT_POST_DISCONNECT || mode==REBOOT_LONG || mode==REBOOT_CANCEL || mode==REBOOT_CANCEL_QUERY;
    if (f.reboots!=1u || result!=(success?H2_H2LOADER_CLI_EXIT_OK:H2_H2LOADER_CLI_EXIT_RUNTIME)) {
        fprintf(stderr,"capture mode=%d result=%d reboots=%u now=%llu error=%s\n",
            mode,result,f.reboots,(unsigned long long)f.now,f.error_output);
    }
    assert(result==(success?H2_H2LOADER_CLI_EXIT_OK:H2_H2LOADER_CLI_EXIT_RUNTIME));
    assert(f.reboots==1u && f.controls==0u);
    assert(f.opens==(mode==REBOOT_DISCONNECT || mode==REBOOT_POST_DISCONNECT || mode==REBOOT_LATE_HANDSHAKE?3u:2u));
    assert(f.closes==f.opens && f.peer==NULL && f.allocations==0u);
    assert(occurrences(f.output,"REBOOT_NATIVE_BOOT\n")==1u);
    assert(strstr(f.output,"PRE_ACK_DATA_UNOBSERVED")==NULL);
    assert(strstr(f.output,"H2_LOADER_STATUS ")==NULL);
    assert(occurrences(f.output,"H2_LOADER_STATU_note actual-console\n")==f.status_emitted);
    if (success && mode!=REBOOT_CANCEL && mode!=REBOOT_CANCEL_QUERY) {
        const char *previous=strstr(f.output,"REBOOT_NATIVE_BOOT\n");
        for (unsigned i=0u;i<8u;++i) {
            char line[48];snprintf(line,sizeof(line),"REBOOT_NATIVE_CASE id=%u\n",i);
            const char *next=strstr(f.output,line);assert(next!=NULL && next>previous);
            assert(occurrences(f.output,line)==1u);previous=next;
        }
        const char *report=strstr(f.output,"REBOOT_NATIVE_REPORT cases=8\n");
        const char *ready=strstr(f.output,"REBOOT_NATIVE_READY\n");
        assert(report!=NULL && ready!=NULL && report>previous && ready>report);
        assert(f.final_status_count!=0u && f.post_status_count>1u);
    }
    if (mode==REBOOT_LONG) assert(f.now>=130000u && f.now<132000u);
    if (mode==REBOOT_CANCEL_QUERY) assert(f.now-f.boot_ms<3000u && f.post_status_count==0u);
    if (mode==REBOOT_FOREVER) {
        assert(f.now>=65000u && f.now<67000u);
        assert(strstr(f.error_output,"command failed code=-6")!=NULL);
    }
    if (mode==REBOOT_LOG_ERROR) assert(strstr(f.error_output,"command failed code=-6")!=NULL);
    if (mode==REBOOT_CLOCK_ERROR) assert(strstr(f.error_output,"command failed code=-4")!=NULL);
    if (mode==REBOOT_LATE_HANDSHAKE || mode==REBOOT_LATE_RESPONSE) {
        assert(f.now>=120000u && f.now<122000u);
        assert(strstr(f.error_output,"command failed code=-6")!=NULL);
    }
    if (mode==REBOOT_BAD_UID || mode==REBOOT_BAD_IMAGE || mode==REBOOT_BAD_P1 || mode==REBOOT_NORMAL_BAD_IMAGE) {
        assert(f.post_status_count==1u && f.native_step<8u);
    }
}

static int host_log(void *user, const uint8_t *data, size_t len) {
    size_t written=0u;
    return output(user,data,len,&written,0u);
}

/* Exercise the real Host UART/KCP dispatch, including each independently
 * blocking phase, and then use the same connection with its legacy budget. */
static void run_status_deadline_case(unsigned mode) {
    cli_fixture_t f={0};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_h2loader_host_serial_connection_config_t config={.serial=&serial,.time=&time,.allocator=&f.mem,
        .port_id="scan-port",.handshake_timeout_ms=1000u,.command_timeout_ms=120000u,
        .preserve_control_lines=1,.on_log=host_log,.log_user=&f};
    if (mode==4u) config.post_command_delay_ms=500u;
    if (mode>=10u) config.command_timeout_ms=1000u;
    h2_h2loader_host_serial_connection_t *connection=NULL;
    assert(h2_h2loader_host_serial_connect(&config,&connection)==H2_PAL_OK);
    f.largest_write_timeout=0u;
    f.trace_reboot=1;
    const int success=mode==0u || mode==6u;
    if (success) { f.status_cr_only=mode==0u?1:2;f.read_limit=17u; }
    if (mode==2u) f.drop_delivery=1;
    if (mode==3u) f.drop_response=1;
    if (mode==5u) f.delay_write=1;
    if (mode==7u) f.fail_write_result=H2_PAL_ERR_WOULD_BLOCK;
    if (mode==8u) f.fail_clock_result=H2_PAL_ERR_WOULD_BLOCK;
    if (mode==9u) f.fail_log_result=H2_PAL_ERR_WOULD_BLOCK;
    if (mode>=10u) {
        f.raw_log_error=1;f.drop_response=1;
        f.fail_log_result=mode==10u?H2_PAL_ERR_WOULD_BLOCK:H2_PAL_ERR_TIMEOUT;
    }
    f.io_deadline=mode==1u?f.now:f.now+(success || mode>=8u?3000u:50u);
    h2_h2loader_host_status_t status;
    memset(&status,0xaa,sizeof(status));
    int pending=1;
    int result=h2_h2loader_host_serial_read_status_until(connection,&status,f.io_deadline,&pending);
    int expected=success?H2_PAL_OK:mode>=7u && mode!=11u?H2_PAL_ERR_WOULD_BLOCK:H2_PAL_ERR_TIMEOUT;
    if (result!=expected) {
        fprintf(stderr,"status deadline mode=%u result=%d now=%llu deadline=%llu requests=%u\n",
            mode,result,(unsigned long long)f.now,(unsigned long long)f.io_deadline,f.requests);
    }
    assert(result==expected && pending==0);
    if (mode>=10u) assert(f.raw_log_emitted && f.requests==1u && f.now<f.raw_log_due+100u);
    assert(f.now<=f.io_deadline+10u);
    if (success) {
        assert(occurrences(f.output,"H2_LOADER_STATU_note actual-console\n")==1u);
        /* The tail follows a CR-only control line and can span UART reads. */
        assert(occurrences(f.output,"STATUS_TAIL_ACTUAL_CASE\n")==1u);
        assert(strstr(f.output,"H2_LOADER_STATUS ")==NULL);
    } else {
        h2_h2loader_host_status_t empty={0};
        assert(memcmp(&status,&empty,sizeof(status))==0);
        if (mode==1u || mode==2u || mode==5u) assert(f.requests==0u);
        if (mode==1u) assert(f.largest_write_timeout==0u);
        if (mode==5u) assert(f.largest_write_timeout<=50u);
    }
    f.drop_delivery=f.drop_response=f.delay_write=f.status_cr_only=0;
    f.fail_write_result=f.fail_clock_result=f.fail_log_result=0;
    f.raw_log_error=0;
    f.read_limit=0u;
    f.largest_write_timeout=0u;
    assert(h2_h2loader_host_serial_read_status(connection,&status)==H2_PAL_OK);
    assert(f.largest_write_timeout==config.command_timeout_ms);
    assert(h2_h2loader_host_serial_disconnect(&connection)==H2_PAL_OK);
    assert(connection==NULL && f.peer==NULL && f.allocations==0u && f.opens==1u && f.closes==1u);
}

static void run_pending_zero_deadline_case(void) {
    cli_fixture_t f={0};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_h2loader_host_serial_connection_config_t config={.serial=&serial,.time=&time,.allocator=&f.mem,
        .port_id="scan-port",.handshake_timeout_ms=1000u,.command_timeout_ms=1000u,
        .preserve_control_lines=1,.on_log=host_log,.log_user=&f};
    h2_h2loader_host_serial_connection_t *connection=NULL;
    assert(h2_h2loader_host_serial_connect(&config,&connection)==H2_PAL_OK);
    f.drop_response=1;
    h2_h2loader_host_status_t status,empty={0};
    int pending=0;
    assert(h2_h2loader_host_serial_read_status_until(connection,&status,f.now+3000u,&pending)==H2_PAL_ERR_WOULD_BLOCK);
    assert(pending==1 && f.requests==1u && memcmp(&status,&empty,sizeof(status))==0);
    size_t allocated=f.allocations;
    assert(h2_h2loader_host_serial_read_status(connection,&status)==H2_PAL_ERR_INVALID_STATE);
    assert(f.requests==1u && f.allocations==allocated);
    uint64_t now=f.now;
    memset(&status,0xaa,sizeof(status));
    assert(h2_h2loader_host_serial_read_status_until(connection,&status,0u,&pending)==H2_PAL_ERR_TIMEOUT);
    assert(pending==0 && f.now==now && f.requests==1u && f.allocations+1u==allocated);
    assert(memcmp(&status,&empty,sizeof(status))==0);
    f.drop_response=0;
    assert(h2_h2loader_host_serial_read_status(connection,&status)==H2_PAL_OK);
    assert(f.requests==2u);
    assert(h2_h2loader_host_serial_disconnect(&connection)==H2_PAL_OK);
    assert(connection==NULL && f.peer==NULL && f.allocations==0u && f.opens==1u && f.closes==1u);
}

static void run_delayed_status_case(int mode) {
    cli_fixture_t f={.mode=REBOOT_PENDING,.trace_reboot=1,.delayed_status=mode};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_iostreamikcp_filter_init(&f.filter);
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_command_io_api_t io={.user=&f,.vtable=&output_vtable};
    h2_command_io_api_t error_io={.user=&f,.vtable=&error_output_vtable};
    h2_runtime_t runtime={.mem=&f.mem,.time=&time,.fs=h2_pal_unsupported_fs_api()};
    const char *argv[]={"h2loader","--port","scan-port","--wait-timeout",
        mode==4 || mode==7?"65":"120","--read-timeout","1","reboot","upgrade","--monitor"};
    h2_h2loader_cli_config_t config={.argc=10,.argv=argv,.serial=&serial,.stdout_io=&io,
        .stderr_io=&error_io,.is_cancelled=cancelled,.cancel_user=&f};
    int result=h2_h2loader_cli_main(&runtime,&config);
    assert(result==(mode==4 || mode==7?H2_H2LOADER_CLI_EXIT_RUNTIME:H2_H2LOADER_CLI_EXIT_OK));
    assert(f.reboots==1u && f.controls==0u);
    assert(f.opens==(mode==5?3u:2u) && f.closes==f.opens);
    assert(f.peer==NULL && f.allocations==0u);
    assert(f.requests==f.request_at_delay+(mode==5?1u:0u));
    assert(f.delayed_requests==(mode==2?2u:1u));
    assert(f.gap_logs==0u);
    assert(strstr(f.output,"H2_LOADER_STATUS ")==NULL && strstr(f.output,"board=devkit")==NULL);
    if (mode==4 || mode==7) {
        assert(f.now>=65000u && f.now<67000u && f.device_logs==0u);
        assert(strstr(f.error_output,"command failed code=-6")!=NULL);
        assert(strstr(f.output,"actual-console\nH2_LOADER_STA")==NULL);
    } else if (mode==6) {
        assert(f.now-f.delayed_started<1250u && f.device_logs==0u);
    } else {
        assert(f.device_logs==1u && f.final_status_count>0u);
        const char *confirm=strstr(f.output,"LIBCO_CONFIRM rc=0\n");
        const char *after=strstr(f.output,"LIBCO_AFTER_CONFIRM rc=0\n");
        const char *ready=strstr(f.output,"LIBCO_READY rc=0\n");
        assert(confirm!=NULL && after>confirm && ready>after);
        assert(occurrences(f.output,"LIBCO_CONFIRM rc=0\n")==1u);
        assert(occurrences(f.output,"LIBCO_AFTER_CONFIRM rc=0\n")==1u);
        assert(occurrences(f.output,"LIBCO_READY rc=0\n")==1u);
        if (mode==3) assert(occurrences(f.output,"STATUS_TAIL_ACTUAL_CASE\n")==1u);
    }
}

static void run_connect_deadline_case(unsigned mode) {
    cli_fixture_t f={.mode=mode==1u?HANDSHAKE_TIMEOUT:HEALTHY};
    f.mem=(h2_pal_mem_api_t){.user=&f,.vtable=&mem_vtable};
    f.uart=(h2_pal_uart_io_stream_api_t){.user=&f,.vtable=&uart_vtable};
    h2_pal_serial_host_api_t serial={.user=&f,.vtable=&serial_vtable};
    h2_pal_time_api_t time={.user=&f,.vtable=&time_vtable};
    h2_h2loader_host_serial_connection_config_t config={.serial=&serial,.time=&time,.allocator=&f.mem,
        .port_id="scan-port",.handshake_timeout_ms=120000u,.command_timeout_ms=120000u,
        .preserve_control_lines=1};
    f.io_deadline=mode==0u?0u:50u;
    if (mode==2u) f.delay_write=1;
    if (mode==3u) config.ready_marker="MISSING_READY";
    h2_h2loader_host_serial_connection_t *connection=NULL;
    assert(h2_h2loader_host_serial_connect_until(&config,&connection,f.io_deadline)==H2_PAL_ERR_TIMEOUT);
    assert(connection==NULL && f.peer==NULL && f.allocations==0u && f.opens==f.closes);
    assert(f.now<=50u && f.largest_write_timeout<=50u);
    if (mode==0u) assert(f.opens==0u);
}

int main(void) {
    for (int mode=HEALTHY; mode<=CLOSE_FAILURE; ++mode) {
        run_case(CLI_SCAN,mode);
        run_case(CLI_STATUS,mode);
    }
    run_case(CLI_MONITOR,HEALTHY);
    run_case(CLI_REBOOT,HEALTHY);
    run_case(CLI_REBOOT,WRONG_REBOOT_ROLE);
    run_wifi_case(0,HEALTHY);
    run_wifi_case(1,HEALTHY);
    run_wifi_case(1,OLD_WIFI_TARGET);
    run_wifi_case(1,WIFI_ERROR);
    run_wifi_case(1,WIFI_LEGACY_REPLY);
    run_wifi_case(2,HEALTHY);
    run_wifi_monitor_case(1,1,HEALTHY,0);
    run_wifi_monitor_case(0,1,HEALTHY,0);
    run_wifi_monitor_case(1,0,HEALTHY,0);
    run_wifi_monitor_case(1,1,WIFI_ERROR,0);
    run_wifi_monitor_case(1,1,OLD_WIFI_TARGET,0);
    run_wifi_monitor_case(1,1,WIFI_HANDSHAKE_LOG_ERROR,0);
    run_wifi_monitor_case(1,1,WIFI_RUNNING_LOG_ERROR,0);
    run_wifi_monitor_case(1,1,WIFI_MONITOR_LOG_ERROR,0);
    run_wifi_monitor_case(1,1,WIFI_MONITOR_READ_ERROR,0);
    run_wifi_monitor_case(1,1,CLOSE_FAILURE,0);
    run_wifi_monitor_case(0,0,HEALTHY,1);
    run_wifi_monitor_case(1,0,HEALTHY,1);
    run_wifi_monitor_case(0,1,HEALTHY,1);
    run_wifi_monitor_case(1,1,HEALTHY,1);
    for (int mode=REBOOT_PENDING;mode<=REBOOT_CANCEL_QUERY;++mode) run_reboot_capture_case(mode);
    for (unsigned mode=0u;mode<=11u;++mode) run_status_deadline_case(mode);
    run_pending_zero_deadline_case();
    for (unsigned mode=0u;mode<=3u;++mode) run_connect_deadline_case(mode);
    for (int mode=1;mode<=7;++mode) run_delayed_status_case(mode);
    return 0;
}
