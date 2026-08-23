#include "h2_bleikcp_speed.h"

#include "internal.h"

#include "h2_bleikcp.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define H2_SPEED_FLUSH_BYTES (64u * 1024u)
#define H2_SPEED_IO_TIMEOUT_MS 5000u
#define H2_SPEED_SCAN_TIMEOUT_MS 5000u
#define H2_SPEED_SCAN_SETTLE_MS 250u
#define H2_SPEED_SCAN_STOP_RETRY_MS 50u
#define H2_SPEED_SCAN_STOP_RETRIES 20u
#define H2_SPEED_CONNECT_SETTLE_MS 500u
#define H2_SPEED_SETUP_TIMEOUT_MS 10000u
#define H2_SPEED_WINDOW 32u
#define H2_SPEED_INPUT_FRAMES 64u
#define H2_SPEED_BUFFER_SIZE (32u * 1024u)
#define H2_SPEED_SAMPLE_COUNT 6u
#define H2_SPEED_ADV_INTERVAL_MIN_MS 100u
#define H2_SPEED_ADV_INTERVAL_MAX_MS 120u
#define H2_SPEED_RENDER_INTERVAL_MS 1000u
#define H2_SPEED_PRESENT_LOGICAL_ROWS 10u

static const uint8_t s_service_uuid[] = { 0xe0u, 0xfeu };
static const uint8_t s_tx_uuid[] = { 0xe1u, 0xfeu };
static const uint8_t s_rx_uuid[] = { 0xe2u, 0xfeu };

/* App-owned 5x7 uppercase diagnostic font; each byte is one five-bit row. */
static const uint8_t s_font_rows[36][7] = {
    { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 },
    { 14, 17, 16, 16, 16, 17, 14 }, { 30, 17, 17, 17, 17, 17, 30 },
    { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 },
    { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 },
    { 14, 4, 4, 4, 4, 4, 14 }, { 7, 2, 2, 2, 18, 18, 12 },
    { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
    { 17, 27, 21, 21, 17, 17, 17 }, { 17, 25, 21, 19, 17, 17, 17 },
    { 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
    { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 },
    { 15, 16, 16, 14, 1, 1, 30 }, { 31, 4, 4, 4, 4, 4, 4 },
    { 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },
    { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
    { 17, 17, 10, 4, 4, 4, 4 }, { 31, 1, 2, 4, 8, 16, 31 },
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
    { 14, 17, 1, 2, 4, 8, 31 }, { 30, 1, 1, 14, 1, 1, 30 },
    { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 16, 30, 1, 1, 30 },
    { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
    { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 },
};

typedef struct h2_speed_sample {
    uint64_t time_ms;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} h2_speed_sample_t;

typedef struct h2_speed_context {
    h2_runtime_t *runtime;
    h2_pal_mutex_t *metrics_mutex;
    h2_bleikcp_speed_role_t role;
    h2_pal_ble_adv_type_t advertising_type;
    h2_pal_ble_scan_type_t scan_type;
    h2_bleikcp_speed_advertising_control_fn pause_management_advertising;
    h2_bleikcp_speed_advertising_control_fn resume_management_advertising;
    h2_bleikcp_speed_advertising_service_fn advertise_server_service;
    void *management_advertising_user;
    h2_bleikcp_speed_ready_fn ready;
    void *ready_user;
    h2_bleikcp_speed_should_stop_fn should_stop;
    void *stop_user;
    bool ready_reported;
    volatile uint64_t tx_total;
    volatile uint64_t rx_total;
    _Atomic int session_stop;
    _Atomic int session_error;
    _Atomic int advertising_restart_pending;
    _Atomic int renderer_stop;
    uint64_t session_id;
    uint16_t conn_handle;
    uint16_t mtu;
    uint16_t interval_ms;
    h2_pal_ble_phy_t phy;
    _Atomic uint32_t connect_attempts;
    _Atomic uint32_t connections;
    _Atomic uint32_t reconnects;
    _Atomic uint32_t disconnects;
    const char *last_error_stage;
    int last_error_code;
    h2_speed_sample_t samples[H2_SPEED_SAMPLE_COUNT];
    size_t sample_count;
    h2_pal_system_event_subscription_t *advertising_subscription;
    h2_pal_system_event_subscription_t *connection_subscription;
    h2_pal_ble_adv_set_t *advertising_set;
    h2_display_info_t display_info;
    uint16_t *framebuffer;
    size_t framebuffer_pixels;
    uint16_t *present_buffer;
    size_t present_buffer_pixels;
    int canvas_width;
    int canvas_height;
    int output_scale;
    int output_x;
    h2_pal_task_t *renderer_task;
    h2_bleikcp_server_t *server;
    char state[16];
    double tx_5s_kib_s;
    double rx_5s_kib_s;
    h2_bleikcp_stats_t stream_stats;
} h2_speed_context_t;

typedef struct h2_speed_screen_snapshot {
    h2_bleikcp_speed_role_t role;
    char state[16];
    uint16_t mtu;
    uint16_t interval_ms;
    h2_pal_ble_phy_t phy;
    uint64_t tx_total;
    uint64_t rx_total;
    double tx_5s_kib_s;
    double rx_5s_kib_s;
    uint32_t connect_attempts;
    uint32_t connections;
    uint32_t reconnects;
    uint32_t disconnects;
    h2_bleikcp_stats_t stream_stats;
    const char *last_error_stage;
    int last_error_code;
} h2_speed_screen_snapshot_t;

typedef struct h2_speed_writer {
    h2_speed_context_t *context;
    h2_bleikcp_t *stream;
    uint8_t direction;
} h2_speed_writer_t;

typedef struct h2_speed_scan {
    h2_speed_context_t *context;
    h2_pal_ble_addr_t addr;
    int rssi;
    bool found;
} h2_speed_scan_t;

static const char *h2_speed_role_name(h2_bleikcp_speed_role_t role);
static const char *h2_speed_phy_name(h2_pal_ble_phy_t phy);
static const char *h2_speed_state_badge(const char *state);

static bool h2_speed_should_stop(const h2_speed_context_t *context) {
    return context->should_stop != NULL &&
        context->should_stop(context->stop_user);
}

static int h2_speed_sleep_interruptible(
    h2_speed_context_t *context,
    uint32_t duration_ms) {
    while (duration_ms > 0u) {
        if (h2_speed_should_stop(context)) {
            return H2_PAL_ERR_CLOSED;
        }
        uint32_t slice_ms = duration_ms > 50u ? 50u : duration_ms;
        int rc = h2_pal_time_sleep_ms(context->runtime->time, slice_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        duration_ms -= slice_ms;
    }
    return H2_PAL_OK;
}

static int h2_speed_mark_ready(h2_speed_context_t *context) {
    if (context->ready_reported) {
        return H2_PAL_OK;
    }
    int rc = context->ready(context->ready_user);
    if (rc == H2_PAL_OK) {
        context->ready_reported = true;
    }
    return rc;
}

static void h2_speed_fill_rect(
    h2_speed_context_t *context,
    int x,
    int y,
    int width,
    int height,
    uint16_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > context->canvas_width ?
        context->canvas_width : x + width;
    int y1 = y + height > context->canvas_height ?
        context->canvas_height : y + height;
    for (int row = y0; row < y1; ++row) {
        uint16_t *pixels = context->framebuffer +
            (size_t)row * (size_t)context->canvas_width;
        for (int column = x0; column < x1; ++column) {
            pixels[column] = color;
        }
    }
}

static void h2_speed_draw_panel(
    h2_speed_context_t *context,
    int x,
    int y,
    int width,
    int height,
    uint16_t fill,
    uint16_t border) {
    h2_speed_fill_rect(context, x, y, width, height, fill);
    h2_speed_fill_rect(context, x, y, width, 1, border);
    h2_speed_fill_rect(context, x, y + height - 1, width, 1, border);
    h2_speed_fill_rect(context, x, y, 1, height, border);
    h2_speed_fill_rect(context, x + width - 1, y, 1, height, border);
}

static const uint8_t *h2_speed_glyph(char character) {
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    if (character >= 'A' && character <= 'Z') {
        return s_font_rows[character - 'A'];
    }
    if (character >= '0' && character <= '9') {
        return s_font_rows[26 + character - '0'];
    }
    return NULL;
}

static void h2_speed_draw_text(
    h2_speed_context_t *context,
    const char *line,
    int x,
    int y,
    uint16_t scale,
    uint16_t color) {
    for (size_t character_index = 0u; line[character_index] != '\0';
         ++character_index) {
        const char character = line[character_index];
        const uint8_t *glyph = h2_speed_glyph(character);
        const int glyph_x = x + (int)character_index * 6 * (int)scale;
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = glyph == NULL ? 0u : glyph[row];
            if (glyph == NULL) {
                if (character == '.' && row == 6) bits = 4u;
                if (character == '-' && row == 3) bits = 14u;
                if (character == '_' && row == 6) bits = 31u;
                if (character == ':' && (row == 2 || row == 5)) bits = 4u;
                if (character == '/' && row >= 1 && row <= 5) {
                    bits = (uint8_t)(1u << (5 - row));
                }
            }
            for (int column = 0; column < 5; ++column) {
                if ((bits & (uint8_t)(1u << (4 - column))) == 0u) continue;
                h2_speed_fill_rect(
                    context,
                    glyph_x + column * (int)scale,
                    y + row * (int)scale,
                    (int)scale,
                    (int)scale,
                    color);
            }
        }
    }
}

static void h2_speed_format_bytes(
    char *out,
    size_t out_size,
    uint64_t bytes) {
    const char *unit = "B";
    double value = (double)bytes;
    if (bytes >= 1024u * 1024u * 1024u) {
        unit = "GiB";
        value /= 1024.0 * 1024.0 * 1024.0;
    } else if (bytes >= 1024u * 1024u) {
        unit = "MiB";
        value /= 1024.0 * 1024.0;
    } else if (bytes >= 1024u) {
        unit = "KiB";
        value /= 1024.0;
    }
    (void)snprintf(out, out_size, "%.2f %s", value, unit);
}

static void h2_speed_screen_snapshot(
    h2_speed_context_t *context,
    h2_speed_screen_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    snapshot->role = context->role;
    memcpy(snapshot->state, context->state, sizeof(snapshot->state));
    snapshot->mtu = context->mtu;
    snapshot->interval_ms = context->interval_ms;
    snapshot->phy = context->phy;
    snapshot->tx_total = context->tx_total;
    snapshot->rx_total = context->rx_total;
    snapshot->tx_5s_kib_s = context->tx_5s_kib_s;
    snapshot->rx_5s_kib_s = context->rx_5s_kib_s;
    snapshot->stream_stats = context->stream_stats;
    snapshot->last_error_stage = context->last_error_stage;
    snapshot->last_error_code = context->last_error_code;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    snapshot->connect_attempts = atomic_load_explicit(
        &context->connect_attempts, memory_order_acquire);
    snapshot->connections = atomic_load_explicit(
        &context->connections, memory_order_acquire);
    snapshot->reconnects = atomic_load_explicit(
        &context->reconnects, memory_order_acquire);
    snapshot->disconnects = atomic_load_explicit(
        &context->disconnects, memory_order_acquire);
}

static int h2_speed_present(h2_speed_context_t *context) {
    if (context->output_scale == 1) {
        const h2_display_rect_t rect = {
            .x = 0,
            .y = 0,
            .width = context->canvas_width,
            .height = context->canvas_height,
        };
        int rc = h2_pal_display_draw_bitmap(
            context->runtime->display, &rect, context->framebuffer,
            (size_t)context->canvas_width * sizeof(*context->framebuffer),
            H2_DISPLAY_PIXEL_RGB565);
        return rc == H2_DISPLAY_OK ?
            h2_pal_display_present(context->runtime->display) : rc;
    }

    const int output_width = context->canvas_width * context->output_scale;
    for (int logical_y = 0; logical_y < context->canvas_height;
         logical_y += (int)H2_SPEED_PRESENT_LOGICAL_ROWS) {
        int logical_rows = context->canvas_height - logical_y;
        if (logical_rows > (int)H2_SPEED_PRESENT_LOGICAL_ROWS) {
            logical_rows = (int)H2_SPEED_PRESENT_LOGICAL_ROWS;
        }
        for (int row = 0; row < logical_rows; ++row) {
            const uint16_t *source = context->framebuffer +
                (size_t)(logical_y + row) * (size_t)context->canvas_width;
            for (int duplicate_y = 0; duplicate_y < context->output_scale;
                 ++duplicate_y) {
                uint16_t *target = context->present_buffer +
                    (size_t)(row * context->output_scale + duplicate_y) *
                    (size_t)output_width;
                for (int column = 0; column < context->canvas_width; ++column) {
                    for (int duplicate_x = 0;
                         duplicate_x < context->output_scale; ++duplicate_x) {
                        target[column * context->output_scale + duplicate_x] =
                            source[column];
                    }
                }
            }
        }
        const h2_display_rect_t rect = {
            .x = context->output_x,
            .y = logical_y * context->output_scale,
            .width = output_width,
            .height = logical_rows * context->output_scale,
        };
        int rc = h2_pal_display_draw_bitmap(
            context->runtime->display, &rect, context->present_buffer,
            (size_t)output_width * sizeof(*context->present_buffer),
            H2_DISPLAY_PIXEL_RGB565);
        if (rc != H2_DISPLAY_OK) return rc;
    }
    return h2_pal_display_present(context->runtime->display);
}

static int h2_speed_render(h2_speed_context_t *context) {
    h2_speed_screen_snapshot_t snapshot;
    h2_speed_screen_snapshot(context, &snapshot);
    memset(context->framebuffer, 0,
        context->framebuffer_pixels * sizeof(*context->framebuffer));

    const bool portrait = context->canvas_height >= 400;
    const uint16_t accent = strcmp(snapshot.state, "running") == 0 ?
        0x07e0u : (snapshot.last_error_code == H2_PAL_OK ? 0x07ffu : 0xfbe0u);
    char line[64];
    char tx_total[24];
    char rx_total[24];
    h2_speed_format_bytes(tx_total, sizeof(tx_total), snapshot.tx_total);
    h2_speed_format_bytes(rx_total, sizeof(rx_total), snapshot.rx_total);

    if (portrait) {
        h2_speed_fill_rect(context, 0, 0, 368, 58, 0x1024u);
        h2_speed_fill_rect(context, 17, 21, 16, 16, accent);
        h2_speed_draw_text(context, "BLE iKCP", 43, 21, 2u, 0xffffu);
        (void)snprintf(line, sizeof(line), "%s %s",
            h2_speed_role_name(snapshot.role), snapshot.state);
        h2_speed_draw_text(context, line, 241, 25, 1u, accent);

        h2_speed_draw_panel(context, 16, 76, 336, 62, 0x08e3u, 0x29e7u);
        (void)snprintf(line, sizeof(line), "PEER %s",
            snapshot.connections > 0u ? "ACTIVE" : "WAITING");
        h2_speed_draw_text(context, line, 31, 90, 1u, 0x94b2u);
        (void)snprintf(line, sizeof(line), "MTU %u", (unsigned)snapshot.mtu);
        h2_speed_draw_text(context, line, 31, 112, 2u, 0xe73cu);
        (void)snprintf(line, sizeof(line), "PHY %s",
            h2_speed_phy_name(snapshot.phy));
        h2_speed_draw_text(context, line, 139, 112, 2u, 0xe73cu);
        (void)snprintf(line, sizeof(line), "INT %ums",
            (unsigned)snapshot.interval_ms);
        h2_speed_draw_text(context, line, 239, 112, 2u, 0xe73cu);

        h2_speed_draw_panel(context, 16, 154, 336, 82, 0x0506u, 0x2b4eu);
        h2_speed_draw_text(context, "TX LAST 5 SECONDS", 31, 172, 1u, 0x07ffu);
        (void)snprintf(line, sizeof(line), "%.1f", snapshot.tx_5s_kib_s);
        h2_speed_draw_text(context, line, 31, 195, 4u, 0xffffu);
        h2_speed_draw_text(context, "KiB/s", 174, 205, 2u, 0x87ffu);
        (void)snprintf(line, sizeof(line), "TOTAL %s", tx_total);
        h2_speed_draw_text(context, line, 237, 210, 1u, 0x07ffu);

        h2_speed_draw_panel(context, 16, 250, 336, 82, 0x1107u, 0x3379u);
        h2_speed_draw_text(context, "RX LAST 5 SECONDS", 31, 268, 1u, 0x7dffu);
        (void)snprintf(line, sizeof(line), "%.1f", snapshot.rx_5s_kib_s);
        h2_speed_draw_text(context, line, 31, 291, 4u, 0xffffu);
        h2_speed_draw_text(context, "KiB/s", 174, 301, 2u, 0xb6ffu);
        (void)snprintf(line, sizeof(line), "TOTAL %s", rx_total);
        h2_speed_draw_text(context, line, 237, 306, 1u, 0x7dffu);

        h2_speed_draw_panel(context, 16, 346, 336, 40, 0x08e3u, 0x08e3u);
        (void)snprintf(line, sizeof(line), "CONN %u", (unsigned)snapshot.connections);
        h2_speed_draw_text(context, line, 29, 362, 1u, 0xc618u);
        (void)snprintf(line, sizeof(line), "RECONN %u", (unsigned)snapshot.reconnects);
        h2_speed_draw_text(context, line, 104, 362, 1u, 0xc618u);
        (void)snprintf(line, sizeof(line), "DISC %u", (unsigned)snapshot.disconnects);
        h2_speed_draw_text(context, line, 212, 362, 1u, 0xc618u);
        (void)snprintf(line, sizeof(line), "RTX %" PRIu64,
            snapshot.stream_stats.retransmits);
        h2_speed_draw_text(context, line, 285, 362, 1u, 0xc618u);
        (void)snprintf(line, sizeof(line), "WAIT %u  HWM %zu/%zu",
            (unsigned)snapshot.stream_stats.waitsnd,
            snapshot.stream_stats.input_high_water,
            snapshot.stream_stats.rx_high_water);
        h2_speed_draw_text(context, line, 29, 376, 1u, 0x94b2u);

        h2_speed_draw_panel(context, 16, 400, 336, 32, 0x2885u, 0x7800u);
        h2_speed_draw_text(context, "LAST ERROR", 29, 413, 1u, 0xfca5u);
        (void)snprintf(line, sizeof(line), "%s %d",
            snapshot.last_error_stage != NULL ? snapshot.last_error_stage : "none",
            snapshot.last_error_code);
        h2_speed_draw_text(context, line, 129, 413, 1u,
            snapshot.last_error_code == H2_PAL_OK ? 0x7befu : 0xf800u);
    } else {
        h2_speed_fill_rect(context, 0, 0, 320, 38, 0x1024u);
        h2_speed_fill_rect(context, 12, 13, 12, 12, accent);
        (void)snprintf(line, sizeof(line), "BLE iKCP %s",
            h2_speed_role_name(snapshot.role));
        h2_speed_draw_text(context, line, 31, 13, 2u, 0xffffu);
        h2_speed_draw_panel(context, 244, 9, 64, 21, 0x1225u, accent);
        h2_speed_draw_text(
            context, h2_speed_state_badge(snapshot.state), 251, 16, 1u,
            accent);

        h2_speed_draw_panel(context, 10, 47, 300, 34, 0x08e3u, 0x29e7u);
        h2_speed_draw_text(context, "PEER", 20, 52, 1u, 0x94b2u);
        h2_speed_draw_text(context,
            snapshot.connections > 0u ? "ACTIVE" : "WAITING", 20, 66, 1u, 0xe73cu);
        h2_speed_draw_text(context, "MTU", 86, 52, 1u, 0x94b2u);
        (void)snprintf(line, sizeof(line), "%u", (unsigned)snapshot.mtu);
        h2_speed_draw_text(context, line, 86, 66, 1u, 0xe73cu);
        h2_speed_draw_text(context, "PHY", 139, 52, 1u, 0x94b2u);
        h2_speed_draw_text(context, h2_speed_phy_name(snapshot.phy), 139, 66, 1u, 0xe73cu);
        h2_speed_draw_text(context, "INTERVAL", 192, 52, 1u, 0x94b2u);
        (void)snprintf(line, sizeof(line), "%ums", (unsigned)snapshot.interval_ms);
        h2_speed_draw_text(context, line, 192, 66, 1u, 0xe73cu);

        h2_speed_draw_panel(context, 10, 90, 145, 58, 0x0506u, 0x2b4eu);
        h2_speed_draw_text(context, "TX LAST 5S", 20, 100, 1u, 0x07ffu);
        (void)snprintf(line, sizeof(line), "%.1f", snapshot.tx_5s_kib_s);
        h2_speed_draw_text(context, line, 20, 118, 3u, 0xffffu);
        h2_speed_draw_text(context, "KiB/s", 105, 130, 1u, 0x87ffu);
        h2_speed_draw_panel(context, 165, 90, 145, 58, 0x1107u, 0x3379u);
        h2_speed_draw_text(context, "RX LAST 5S", 175, 100, 1u, 0x7dffu);
        (void)snprintf(line, sizeof(line), "%.1f", snapshot.rx_5s_kib_s);
        h2_speed_draw_text(context, line, 175, 118, 3u, 0xffffu);
        h2_speed_draw_text(context, "KiB/s", 260, 130, 1u, 0xb6ffu);

        h2_speed_draw_panel(context, 10, 157, 300, 28, 0x08e3u, 0x08e3u);
        (void)snprintf(line, sizeof(line), "TOTAL TX %s", tx_total);
        h2_speed_draw_text(context, line, 20, 167, 1u, 0x07ffu);
        (void)snprintf(line, sizeof(line), "RX %s", rx_total);
        h2_speed_draw_text(context, line, 180, 167, 1u, 0x7dffu);

        (void)snprintf(line, sizeof(line), "CONN %u  RE %u  DISC %u  RTX %" PRIu64,
            (unsigned)snapshot.connections, (unsigned)snapshot.reconnects,
            (unsigned)snapshot.disconnects, snapshot.stream_stats.retransmits);
        h2_speed_draw_text(context, line, 12, 193, 1u, 0xc618u);
        (void)snprintf(line, sizeof(line), "WAIT %u  HWM %zu/%zu/%zu",
            (unsigned)snapshot.stream_stats.waitsnd,
            snapshot.stream_stats.input_high_water,
            snapshot.stream_stats.tx_high_water,
            snapshot.stream_stats.rx_high_water);
        h2_speed_draw_text(context, line, 12, 203, 1u, 0xc618u);

        h2_speed_draw_panel(context, 10, 213, 300, 19, 0x2885u, 0x7800u);
        h2_speed_draw_text(context, "LAST ERROR", 18, 219, 1u, 0xfca5u);
        (void)snprintf(line, sizeof(line), "%s %d",
            snapshot.last_error_stage != NULL ? snapshot.last_error_stage : "none",
            snapshot.last_error_code);
        h2_speed_draw_text(context, line, 88, 219, 1u,
            snapshot.last_error_code == H2_PAL_OK ? 0x7befu : 0xf800u);
    }

    return h2_speed_present(context);
}

static void h2_speed_renderer_task(void *user) {
    h2_speed_context_t *context = user;
    while (atomic_load_explicit(
               &context->renderer_stop, memory_order_acquire) == 0) {
        int rc = h2_speed_render(context);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_lock(
                context->runtime->sync, context->metrics_mutex);
            context->last_error_stage = "display";
            context->last_error_code = rc;
            (void)h2_pal_mutex_unlock(
                context->runtime->sync, context->metrics_mutex);
        }
        (void)h2_pal_time_sleep_ms(
            context->runtime->time, H2_SPEED_RENDER_INTERVAL_MS);
    }
}

static int h2_speed_start_display(h2_speed_context_t *context) {
    int rc = h2_pal_display_open(context->runtime->display);
    if (rc != H2_DISPLAY_OK) return rc;
    rc = h2_pal_display_get_info(
        context->runtime->display, &context->display_info);
    if (rc != H2_DISPLAY_OK || context->display_info.width <= 0 ||
        context->display_info.height <= 0) {
        return rc == H2_DISPLAY_OK ? H2_PAL_ERR_INVALID_STATE : rc;
    }
    context->canvas_width = context->display_info.width;
    context->canvas_height = context->display_info.height;
    context->output_scale = 1;
    context->output_x = 0;
    if (context->display_info.width >= 640 &&
        context->display_info.height >= 480) {
        context->canvas_width = 320;
        context->canvas_height = 240;
        context->output_scale = 2;
        context->output_x = (context->display_info.width - 640) / 2;
    }
    if ((size_t)context->canvas_width > SIZE_MAX /
        (size_t)context->canvas_height) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    context->framebuffer_pixels = (size_t)context->canvas_width *
        (size_t)context->canvas_height;
    if (context->framebuffer_pixels > SIZE_MAX / sizeof(*context->framebuffer)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    context->framebuffer = h2_pal_mem_alloc(
        context->runtime->mem,
        context->framebuffer_pixels * sizeof(*context->framebuffer));
    if (context->framebuffer == NULL) return H2_PAL_ERR_NO_MEMORY;
    if (context->output_scale > 1) {
        context->present_buffer_pixels =
            (size_t)context->canvas_width * (size_t)context->output_scale *
            H2_SPEED_PRESENT_LOGICAL_ROWS * (size_t)context->output_scale;
        context->present_buffer = h2_pal_mem_alloc(
            context->runtime->mem,
            context->present_buffer_pixels * sizeof(*context->present_buffer));
        if (context->present_buffer == NULL) return H2_PAL_ERR_NO_MEMORY;
    }
    (void)h2_pal_display_set_brightness_percent(context->runtime->display, 100u);
    rc = h2_speed_render(context);
    return rc;
}

static int h2_speed_start_renderer(h2_speed_context_t *context) {
    const h2_pal_task_options_t options = {
        .name = "bleikcp-speed/ui",
        .min_stack_size = 8u * 1024u,
    };
    atomic_store_explicit(&context->renderer_stop, 0, memory_order_release);
    return h2_pal_task_start(
        context->runtime->task, &options, h2_speed_renderer_task,
        context, &context->renderer_task);
}

static int h2_speed_stop_renderer(h2_speed_context_t *context) {
    if (context->renderer_task == NULL) {
        return H2_PAL_OK;
    }
    atomic_store_explicit(&context->renderer_stop, 1, memory_order_release);
    int rc = h2_pal_task_join(
        context->runtime->task, context->renderer_task);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    context->renderer_task = NULL;
    return H2_PAL_OK;
}

static int h2_speed_cleanup_context(h2_speed_context_t *context) {
    if (context == NULL) return H2_PAL_OK;
    int rc = h2_speed_stop_renderer(context);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (context->connection_subscription != NULL) {
        h2_pal_system_event_unsubscribe(
            context->runtime->system_event,
            context->connection_subscription);
        context->connection_subscription = NULL;
    }
    if (context->advertising_subscription != NULL) {
        h2_pal_system_event_unsubscribe(
            context->runtime->system_event,
            context->advertising_subscription);
        context->advertising_subscription = NULL;
    }
    if (context->advertising_set != NULL) {
        (void)h2_pal_ble_adv_set_stop(
            context->runtime->ble_host, context->advertising_set);
        (void)h2_pal_ble_adv_set_destroy(
            context->runtime->ble_host, context->advertising_set);
        context->advertising_set = NULL;
    }
    if (context->server != NULL) {
        int server_rc = h2_bleikcp_server_close(context->server);
        if (server_rc != H2_PAL_OK) {
            /* The server task may still hold context; preserve it on failure. */
            return server_rc;
        }
        context->server = NULL;
    }
    if (context->present_buffer != NULL) {
        h2_pal_mem_free(context->runtime->mem, context->present_buffer);
    }
    if (context->framebuffer != NULL) {
        h2_pal_mem_free(context->runtime->mem, context->framebuffer);
    }
    (void)h2_pal_display_close(context->runtime->display);
    if (context->metrics_mutex != NULL) {
        (void)h2_pal_mutex_destroy(
            context->runtime->sync, context->metrics_mutex);
    }
    h2_pal_mem_free(context->runtime->mem, context);
    return rc;
}

static int h2_speed_cleanup_error(
    h2_speed_context_t *context,
    int error) {
    int cleanup_rc = h2_speed_cleanup_context(context);
    return cleanup_rc != H2_PAL_OK ? cleanup_rc : error;
}

static void h2_speed_add_total(
    h2_speed_context_t *context,
    uint64_t tx_bytes,
    uint64_t rx_bytes) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    context->tx_total += tx_bytes;
    context->rx_total += rx_bytes;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
}

static void h2_speed_get_totals(
    h2_speed_context_t *context,
    uint64_t *out_tx_bytes,
    uint64_t *out_rx_bytes) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    *out_tx_bytes = context->tx_total;
    *out_rx_bytes = context->rx_total;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
}

static void h2_speed_set_error(
    h2_speed_context_t *context,
    const char *stage,
    int code) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    context->last_error_stage = stage;
    context->last_error_code = code;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    atomic_store_explicit(&context->session_error, code, memory_order_release);
    atomic_store_explicit(&context->session_stop, 1, memory_order_release);
}

static void h2_speed_set_link_metrics(
    h2_speed_context_t *context,
    uint16_t mtu,
    uint16_t interval_ms,
    h2_pal_ble_phy_t phy) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    if (mtu != 0u) {
        context->mtu = mtu;
    }
    if (interval_ms != 0u) {
        context->interval_ms = interval_ms;
    }
    context->phy = phy;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
}

static void h2_speed_reset_link_metrics(h2_speed_context_t *context) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    context->mtu = 0u;
    context->interval_ms = 0u;
    context->phy = H2_PAL_BLE_PHY_UNKNOWN;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
}

static void h2_speed_log_state(
    h2_speed_context_t *context,
    const char *state,
    const char *stage,
    int code) {
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    (void)snprintf(context->state, sizeof(context->state), "%s", state);
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    char line[192];
    (void)snprintf(
        line, sizeof(line),
        "H2_BLEIKCP_SPEED role=%s state=%s stage=%s code=%d "
        "attempts=%u connections=%u reconnects=%u disconnects=%u",
        h2_speed_role_name(context->role), state, stage, code,
        (unsigned)atomic_load_explicit(
            &context->connect_attempts, memory_order_acquire),
        (unsigned)atomic_load_explicit(
            &context->connections, memory_order_acquire),
        (unsigned)atomic_load_explicit(
            &context->reconnects, memory_order_acquire),
        (unsigned)atomic_load_explicit(
            &context->disconnects, memory_order_acquire));
    (void)h2_pal_log_write(
        context->runtime->log,
        code == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "bleikcp-speed", line);
}

static void h2_speed_preserve_server_error(
    h2_speed_context_t *context,
    const char *stage,
    int error) {
    /*
     * The PAL GATT registry is shared with the H2Loader management service.
     * Closing this server would unregister both services. Keep the registered
     * server and its callback context alive so serial/BLE recovery remains
     * available until the launcher reboots the image.
    */
    h2_speed_set_error(context, stage, error);
    h2_speed_log_state(context, "error", stage, error);
}

static int h2_speed_wait_for_stop(h2_speed_context_t *context) {
    while (!h2_speed_should_stop(context)) {
        int rc = h2_speed_sleep_interruptible(context, 1000u);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_CLOSED) {
            return h2_speed_cleanup_error(context, rc);
        }
        if (atomic_exchange_explicit(
                &context->advertising_restart_pending, 0,
                memory_order_acq_rel) != 0 &&
            context->advertising_set != NULL) {
            int advertising_rc = h2_pal_ble_adv_set_start(
                context->runtime->ble_host, context->advertising_set);
            if (advertising_rc != H2_PAL_OK &&
                advertising_rc != H2_PAL_ERR_INVALID_STATE) {
                h2_speed_set_error(
                    context, "advertising", advertising_rc);
                h2_speed_log_state(
                    context, "error", "advertising", advertising_rc);
            }
        }
    }
    return h2_speed_cleanup_context(context);
}

static void h2_speed_writer_task(void *user) {
    h2_speed_writer_t *writer = user;
    h2_speed_context_t *context = writer->context;
    uint8_t payload[H2_SPEED_CHUNK_SIZE];
    uint64_t offset = 0u;
    uint64_t pending = 0u;

    while (atomic_load_explicit(
               &context->session_stop, memory_order_acquire) == 0 &&
           !h2_speed_should_stop(context)) {
        h2_speed_fill_payload(
            payload, sizeof(payload), context->session_id,
            writer->direction, offset);
        int rc;
        do {
            rc = h2_bleikcp_write(
                writer->stream, payload, sizeof(payload),
                H2_SPEED_IO_TIMEOUT_MS);
        } while (h2_speed_io_should_retry(rc) &&
                 atomic_load_explicit(
                     &context->session_stop, memory_order_acquire) == 0 &&
                 !h2_speed_should_stop(context));
        if (h2_speed_io_should_retry(rc) &&
            (atomic_load_explicit(
                 &context->session_stop, memory_order_acquire) != 0 ||
             h2_speed_should_stop(context))) {
            break;
        }
        if (rc != H2_PAL_OK) {
            h2_speed_set_error(context, "io", rc);
            break;
        }
        offset += sizeof(payload);
        pending += sizeof(payload);
        if (pending >= H2_SPEED_FLUSH_BYTES) {
            do {
                rc = h2_bleikcp_flush(
                    writer->stream, H2_SPEED_IO_TIMEOUT_MS);
            } while (h2_speed_io_should_retry(rc) &&
                     atomic_load_explicit(
                         &context->session_stop, memory_order_acquire) == 0 &&
                     !h2_speed_should_stop(context));
            if (h2_speed_io_should_retry(rc) &&
                (atomic_load_explicit(
                     &context->session_stop, memory_order_acquire) != 0 ||
                 h2_speed_should_stop(context))) {
                break;
            }
            if (rc != H2_PAL_OK) {
                h2_speed_set_error(context, "io", rc);
                break;
            }
            h2_speed_add_total(context, pending, 0u);
            pending = 0u;
        }
    }
}

static const char *h2_speed_role_name(h2_bleikcp_speed_role_t role) {
    return role == H2_BLEIKCP_SPEED_ROLE_CLIENT ? "client" : "server";
}

static const char *h2_speed_state_badge(const char *state) {
    if (strcmp(state, "running") == 0) return "RUNNING";
    if (strcmp(state, "advertising") == 0) return "READY";
    if (strcmp(state, "scanning") == 0) return "SCAN";
    if (strcmp(state, "connecting") == 0) return "CONNECT";
    if (strcmp(state, "backoff") == 0) return "RETRY";
    return "ERROR";
}

static const char *h2_speed_phy_name(h2_pal_ble_phy_t phy) {
    switch (phy) {
    case H2_PAL_BLE_PHY_1M:
        return "1m";
    case H2_PAL_BLE_PHY_2M:
        return "2m";
    case H2_PAL_BLE_PHY_CODED:
        return "coded";
    default:
        return "unknown";
    }
}

static void h2_speed_report(
    h2_speed_context_t *context,
    h2_bleikcp_t *stream,
    uint64_t now_ms) {
    uint64_t tx_total = 0u;
    uint64_t rx_total = 0u;
    h2_speed_get_totals(context, &tx_total, &rx_total);
    h2_speed_sample_t current = { now_ms, tx_total, rx_total };
    if (context->sample_count < H2_SPEED_SAMPLE_COUNT) {
        context->samples[context->sample_count++] = current;
    } else {
        memmove(
            &context->samples[0], &context->samples[1],
            (H2_SPEED_SAMPLE_COUNT - 1u) * sizeof(context->samples[0]));
        context->samples[H2_SPEED_SAMPLE_COUNT - 1u] = current;
    }
    const h2_speed_sample_t *oldest = &context->samples[0];
    uint64_t elapsed_ms = now_ms - oldest->time_ms;
    double tx_kib_s = h2_speed_rate_kib_s(
        tx_total, oldest->tx_bytes, elapsed_ms);
    double rx_kib_s = h2_speed_rate_kib_s(
        rx_total, oldest->rx_bytes, elapsed_ms);
    h2_bleikcp_stats_t stats = { 0 };
    (void)h2_bleikcp_get_stats(stream, &stats);
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    if (stats.att_mtu != 0u) {
        context->mtu = stats.att_mtu;
    }
    context->tx_5s_kib_s = tx_kib_s;
    context->rx_5s_kib_s = rx_kib_s;
    context->stream_stats = stats;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    h2_speed_screen_snapshot_t snapshot;
    h2_speed_screen_snapshot(context, &snapshot);
    char line[512];
    (void)snprintf(
        line, sizeof(line),
        "H2_BLEIKCP_SPEED role=%s state=running mtu=%u phy=%s "
        "interval_ms=%u tx_5s_kib_s=%.1f rx_5s_kib_s=%.1f "
        "tx_total=%" PRIu64 " rx_total=%" PRIu64 " connections=%u "
        "reconnects=%u disconnects=%u tx_frames=%" PRIu64 " "
        "rx_frames=%" PRIu64 " retransmits=%" PRIu64 " waitsnd=%u "
        "input_hwm=%zu tx_hwm=%zu rx_hwm=%zu last_error_stage=%s "
        "last_error_code=%d",
        h2_speed_role_name(snapshot.role), (unsigned)snapshot.mtu,
        h2_speed_phy_name(snapshot.phy), (unsigned)snapshot.interval_ms,
        tx_kib_s, rx_kib_s, tx_total, rx_total,
        (unsigned)snapshot.connections, (unsigned)snapshot.reconnects,
        (unsigned)snapshot.disconnects, snapshot.stream_stats.tx_frames,
        snapshot.stream_stats.rx_frames, snapshot.stream_stats.retransmits,
        (unsigned)snapshot.stream_stats.waitsnd,
        snapshot.stream_stats.input_high_water,
        snapshot.stream_stats.tx_high_water,
        snapshot.stream_stats.rx_high_water,
        snapshot.last_error_stage != NULL ? snapshot.last_error_stage : "none",
        snapshot.last_error_code);
    (void)h2_pal_log_write(
        context->runtime->log, H2_PAL_LOG_INFO, "bleikcp-speed", line);
}

static int h2_speed_transfer(
    h2_speed_context_t *context,
    h2_bleikcp_t *stream,
    uint8_t tx_direction) {
    h2_speed_writer_t writer = { context, stream, tx_direction };
    h2_pal_task_t *writer_task = NULL;
    h2_pal_task_options_t options = {
        .name = "bleikcp-speed/tx",
        .min_stack_size = 12u * 1024u,
    };
    uint8_t payload[H2_SPEED_CHUNK_SIZE];
    uint64_t rx_offset = 0u;
    uint64_t last_report_ms = 0u;
    context->sample_count = 0u;
    atomic_store_explicit(&context->session_stop, 0, memory_order_release);
    atomic_store_explicit(
        &context->session_error, H2_PAL_OK, memory_order_release);
    int rc = h2_pal_task_start(
        context->runtime->task, &options, h2_speed_writer_task,
        &writer, &writer_task);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    while (atomic_load_explicit(
               &context->session_stop, memory_order_acquire) == 0 &&
           !h2_speed_should_stop(context)) {
        size_t read_len = 0u;
        do {
            rc = h2_bleikcp_read(
                stream, payload, sizeof(payload), &read_len,
                H2_SPEED_IO_TIMEOUT_MS);
        } while (h2_speed_io_should_retry(rc) &&
                 atomic_load_explicit(
                     &context->session_stop, memory_order_acquire) == 0 &&
                 !h2_speed_should_stop(context));
        if (h2_speed_io_should_retry(rc) &&
            (atomic_load_explicit(
                 &context->session_stop, memory_order_acquire) != 0 ||
             h2_speed_should_stop(context))) {
            rc = H2_PAL_OK;
            break;
        }
        if (rc != H2_PAL_OK) {
            h2_speed_set_error(context, "io", rc);
            break;
        }
        rc = h2_speed_verify_payload(
            payload, read_len, context->session_id,
            (uint8_t)(tx_direction ^ 1u), rx_offset);
        if (rc != H2_PAL_OK) {
            h2_speed_set_error(context, "verify", rc);
            break;
        }
        rx_offset += read_len;
        h2_speed_add_total(context, 0u, read_len);
        uint64_t now_ms = 0u;
        if (h2_pal_time_get_monotonic_ms(context->runtime->time, &now_ms) == H2_PAL_OK &&
            (last_report_ms == 0u || now_ms - last_report_ms >= 1000u)) {
            h2_speed_report(context, stream, now_ms);
            last_report_ms = now_ms;
        }
    }
    atomic_store_explicit(&context->session_stop, 1, memory_order_release);
    int join_rc = h2_pal_task_join(context->runtime->task, writer_task);
    if (join_rc != H2_PAL_OK) {
        return join_rc;
    }
    return atomic_load_explicit(
        &context->session_error, memory_order_acquire);
}

static int h2_speed_server_handler(
    void *user,
    h2_bleikcp_t *stream,
    uint16_t conn_handle) {
    h2_speed_context_t *context = user;
    uint8_t header[H2_SPEED_HEADER_SIZE];
    size_t read_len = 0u;
    uint64_t session_id = 0u;
    (void)atomic_fetch_add_explicit(
        &context->connect_attempts, 1u, memory_order_acq_rel);
    context->conn_handle = conn_handle;
    h2_speed_log_state(context, "connected", "accept", H2_PAL_OK);
    int rc = h2_bleikcp_read(
        stream, header, sizeof(header), &read_len, H2_SPEED_SETUP_TIMEOUT_MS);
    if (rc == H2_PAL_OK && read_len == sizeof(header)) {
        rc = h2_speed_validate_header(header, false, &session_id);
    } else if (rc == H2_PAL_OK) {
        rc = H2_PAL_ERR_FORMAT;
    }
    const int validation_rc = rc;
    h2_speed_make_header(
        header, true, session_id, H2_SPEED_CHUNK_SIZE, validation_rc);
    rc = h2_bleikcp_write(
        stream, header, sizeof(header), H2_SPEED_SETUP_TIMEOUT_MS);
    if (rc == H2_PAL_OK) {
        rc = h2_bleikcp_flush(stream, H2_SPEED_SETUP_TIMEOUT_MS);
    }
    if (validation_rc != H2_PAL_OK) {
        rc = validation_rc;
    }
    if (rc != H2_PAL_OK) {
        h2_speed_set_error(context, "stream", rc);
        h2_speed_log_state(context, "error", "handshake", rc);
        return rc;
    }
    context->session_id = session_id;
    h2_pal_ble_phy_info_t phy = { 0 };
    h2_pal_ble_phy_t actual_phy = H2_PAL_BLE_PHY_UNKNOWN;
    if (h2_pal_ble_read_phy(
            context->runtime->ble_host, conn_handle, &phy,
            H2_SPEED_SETUP_TIMEOUT_MS) == H2_PAL_OK) {
        actual_phy = phy.tx_phy;
    }
    h2_speed_set_link_metrics(context, 0u, 0u, actual_phy);
    if (atomic_load_explicit(
            &context->connections, memory_order_acquire) > 0u) {
        (void)atomic_fetch_add_explicit(
            &context->reconnects, 1u, memory_order_acq_rel);
    }
    (void)atomic_fetch_add_explicit(
        &context->connections, 1u, memory_order_acq_rel);
    h2_speed_log_state(context, "running", "handshake", H2_PAL_OK);
    rc = h2_speed_transfer(context, stream, 1u);
    (void)atomic_fetch_add_explicit(
        &context->disconnects, 1u, memory_order_acq_rel);
    h2_speed_log_state(context, "disconnected", "transfer", rc);
    return rc;
}

static bool h2_speed_scan_result(
    void *user,
    const h2_pal_ble_scan_result_t *result) {
    h2_speed_scan_t *scan = user;
    bool has_service = false;
    for (size_t i = 0u; i < result->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &result->service_uuids[i];
        if (uuid->len == sizeof(s_service_uuid) &&
            memcmp(uuid->data, s_service_uuid, sizeof(s_service_uuid)) == 0) {
            has_service = true;
            break;
        }
    }
    if (!result->connectable || result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE ||
        !has_service) {
        return false;
    }
    (void)h2_pal_mutex_lock(
        scan->context->runtime->sync, scan->context->metrics_mutex);
    if (!scan->found || result->rssi > scan->rssi) {
        scan->addr = result->addr;
        scan->rssi = result->rssi;
        scan->found = true;
    }
    (void)h2_pal_mutex_unlock(
        scan->context->runtime->sync, scan->context->metrics_mutex);
    return false;
}

static int h2_speed_server_system_event(
    void *user,
    const h2_pal_system_event_t *event) {
    h2_speed_context_t *context = user;
    if (context == NULL || event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED &&
        context->advertising_set != NULL) {
        atomic_store_explicit(
            &context->advertising_restart_pending, 1, memory_order_release);
    }
    return H2_PAL_OK;
}

static int h2_speed_connection_system_event(
    void *user,
    const h2_pal_system_event_t *event) {
    h2_speed_context_t *context = user;
    if (context == NULL || event == NULL ||
        event->type != H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED ||
        event->payload == NULL ||
        event->payload_size != sizeof(h2_pal_ble_connection_params_t)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_connection_params_t *params = event->payload;
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    context->interval_ms = params->interval_min_ms;
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    return H2_PAL_OK;
}

static int h2_speed_server_start_advertising(
    h2_speed_context_t *context) {
    const h2_pal_ble_uuid_t service_uuid = {
        .data = s_service_uuid,
        .len = sizeof(s_service_uuid),
    };
    if (context->advertise_server_service != NULL) {
        return context->advertise_server_service(
            context->management_advertising_user, &service_uuid);
    }
    const h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = H2_SPEED_ADV_INTERVAL_MIN_MS,
        .interval_max_ms = H2_SPEED_ADV_INTERVAL_MAX_MS,
        .type = context->advertising_type,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_2M,
        .sid = 1u,
    };
    int rc = h2_pal_ble_adv_set_create(
        context->runtime->ble_host, &params, &context->advertising_set);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_pal_ble_adv_data_t data = {
        .local_name = "H2Loader-BLE-Speed",
        .service_uuids = &service_uuid,
        .service_uuid_count = 1u,
    };
    rc = h2_pal_ble_adv_set_set_data(
        context->runtime->ble_host, context->advertising_set, &data);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_system_event_subscribe(
            context->runtime->system_event,
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
            h2_speed_server_system_event,
            context,
            &context->advertising_subscription);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_start(
            context->runtime->ble_host, context->advertising_set);
    }
    return rc;
}

static int h2_speed_find_peer(
    h2_speed_context_t *context,
    h2_pal_ble_addr_t *out_addr) {
    h2_speed_scan_t scan = { .context = context };
    h2_pal_ble_scan_params_t params = {
        .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
        .interval_ms = 50u,
        .window_ms = 50u,
        .timeout_ms = H2_SPEED_SCAN_TIMEOUT_MS,
        .type = context->scan_type,
        .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
    };
    int rc = h2_pal_ble_start_scan(
        context->runtime->ble_host, &params, h2_speed_scan_result, &scan);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_speed_mark_ready(context);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_ble_stop_scan(context->runtime->ble_host);
        return rc;
    }
    rc = h2_speed_sleep_interruptible(
        context, H2_SPEED_SCAN_TIMEOUT_MS + H2_SPEED_SCAN_SETTLE_MS);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_ble_stop_scan(context->runtime->ble_host);
        return rc;
    }
    for (uint32_t attempt = 0u; attempt < H2_SPEED_SCAN_STOP_RETRIES; ++attempt) {
        rc = h2_pal_ble_stop_scan(context->runtime->ble_host);
        if (rc == H2_PAL_OK || rc == H2_PAL_ERR_INVALID_STATE) {
            rc = H2_PAL_OK;
            break;
        }
        (void)h2_speed_sleep_interruptible(
            context, H2_SPEED_SCAN_STOP_RETRY_MS);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_speed_sleep_interruptible(context, H2_SPEED_CONNECT_SETTLE_MS);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)h2_pal_mutex_lock(context->runtime->sync, context->metrics_mutex);
    const bool found = scan.found;
    if (found) {
        *out_addr = scan.addr;
    }
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->metrics_mutex);
    if (!found) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    return H2_PAL_OK;
}

static h2_bleikcp_api_t h2_speed_api(h2_runtime_t *runtime) {
    return (h2_bleikcp_api_t){
        .ble = runtime->ble_host,
        .task = runtime->task,
        .time = runtime->time,
        .sync = runtime->sync,
        .system_event = runtime->system_event,
        .allocator = runtime->mem,
    };
}

static h2_bleikcp_config_t h2_speed_config(h2_speed_context_t *context) {
    return (h2_bleikcp_config_t){
        .service_uuid = { s_service_uuid, sizeof(s_service_uuid) },
        .tx_char_uuid = { s_tx_uuid, sizeof(s_tx_uuid) },
        .rx_char_uuid = { s_rx_uuid, sizeof(s_rx_uuid) },
        .send_window = H2_SPEED_WINDOW,
        .recv_window = H2_SPEED_WINDOW,
        .input_frame_capacity = H2_SPEED_INPUT_FRAMES,
        .tx_buffer_size = H2_SPEED_BUFFER_SIZE,
        .rx_buffer_size = H2_SPEED_BUFFER_SIZE,
        .no_congestion_control = 0,
        .output_retry_count = 40u,
        .output_retry_delay_ms = 2u,
        .setup_timeout_ms = H2_SPEED_SETUP_TIMEOUT_MS,
        .worker_task_options = { "bleikcp-speed/kcp", 12u * 1024u },
        .server_task_options = { "bleikcp-speed/server", 12u * 1024u },
        .user = context,
    };
}

static int h2_speed_run_client(h2_speed_context_t *context) {
    uint32_t backoff_ms = 250u;
    while (!h2_speed_should_stop(context)) {
        h2_pal_ble_addr_t peer;
        int rc = context->pause_management_advertising(
            context->management_advertising_user);
        if (rc != H2_PAL_OK) {
            h2_speed_set_error(context, "advertising", rc);
            h2_speed_log_state(context, "retry", "advertising", rc);
            (void)h2_speed_sleep_interruptible(context, backoff_ms);
            backoff_ms = h2_speed_next_backoff_ms(backoff_ms);
            continue;
        }
        bool management_advertising_paused = true;
        h2_speed_log_state(context, "scanning", "scan", H2_PAL_OK);
        rc = h2_speed_find_peer(context, &peer);
        if (rc != H2_PAL_OK) {
            int resume_rc = context->resume_management_advertising(
                context->management_advertising_user);
            management_advertising_paused = false;
            if (resume_rc != H2_PAL_OK) {
                rc = resume_rc;
            }
            h2_speed_set_error(context, "scan", rc);
            h2_speed_log_state(context, "retry", "scan", rc);
            (void)h2_speed_sleep_interruptible(context, backoff_ms);
            backoff_ms = h2_speed_next_backoff_ms(backoff_ms);
            continue;
        }
        h2_speed_log_state(context, "found", "scan", H2_PAL_OK);
        (void)atomic_fetch_add_explicit(
            &context->connect_attempts, 1u, memory_order_acq_rel);
        h2_pal_ble_connect_params_t connect_params = {
            .timeout_ms = H2_SPEED_SETUP_TIMEOUT_MS,
            .interval_min_ms = 15u,
            .interval_max_ms = 15u,
            .latency = 0u,
            .supervision_timeout_ms = 4000u,
        };
        uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        rc = h2_pal_ble_connect(
            context->runtime->ble_host, &peer, &connect_params, &conn_handle);
        if (rc != H2_PAL_OK) {
            int resume_rc = context->resume_management_advertising(
                context->management_advertising_user);
            management_advertising_paused = false;
            if (resume_rc != H2_PAL_OK) {
                rc = resume_rc;
            }
            h2_speed_set_error(context, "connect", rc);
            h2_speed_log_state(context, "retry", "connect", rc);
            (void)h2_speed_sleep_interruptible(context, backoff_ms);
            backoff_ms = h2_speed_next_backoff_ms(backoff_ms);
            continue;
        }
        context->conn_handle = conn_handle;
        h2_speed_reset_link_metrics(context);
        h2_speed_log_state(context, "connected", "connect", H2_PAL_OK);
        h2_pal_ble_connection_params_t link = { 15u, 15u, 0u, 4000u };
        (void)h2_pal_ble_update_connection(context->runtime->ble_host, conn_handle, &link);
        uint16_t mtu = 0u;
        rc = h2_pal_ble_exchange_mtu(
            context->runtime->ble_host, conn_handle, &mtu,
            H2_SPEED_SETUP_TIMEOUT_MS);
        if (rc == H2_PAL_OK) {
            h2_speed_set_link_metrics(
                context, mtu, 0u, H2_PAL_BLE_PHY_UNKNOWN);
            h2_speed_log_state(context, "ready", "mtu", H2_PAL_OK);
            int phy_rc = h2_pal_ble_set_preferred_phy(
                context->runtime->ble_host, conn_handle,
                H2_PAL_BLE_PHY_2M, H2_PAL_BLE_PHY_2M,
                H2_SPEED_SETUP_TIMEOUT_MS);
            h2_pal_ble_phy_info_t phy = { 0 };
            if (phy_rc == H2_PAL_OK) {
                phy_rc = h2_pal_ble_read_phy(
                    context->runtime->ble_host, conn_handle, &phy,
                    H2_SPEED_SETUP_TIMEOUT_MS);
            }
            if (phy_rc == H2_PAL_OK) {
                h2_speed_set_link_metrics(context, 0u, 0u, phy.tx_phy);
                h2_speed_log_state(context, "ready", "phy", H2_PAL_OK);
            } else {
                h2_speed_set_link_metrics(
                    context, 0u, 0u, H2_PAL_BLE_PHY_UNKNOWN);
                h2_speed_log_state(context, "warning", "phy", phy_rc);
            }
        }
        h2_bleikcp_t *stream = NULL;
        h2_bleikcp_api_t api = h2_speed_api(context->runtime);
        h2_bleikcp_config_t config = h2_speed_config(context);
        if (rc == H2_PAL_OK) {
            rc = h2_bleikcp_client_open(
                &api, &config, conn_handle, mtu, &stream);
            if (rc == H2_PAL_OK) {
                h2_speed_log_state(context, "ready", "stream", H2_PAL_OK);
            }
        }
        if (management_advertising_paused) {
            int resume_rc = context->resume_management_advertising(
                context->management_advertising_user);
            if (resume_rc == H2_PAL_OK) {
                management_advertising_paused = false;
            } else {
                h2_speed_log_state(
                    context, "warning", "advertising", resume_rc);
            }
        }
        uint8_t header[H2_SPEED_HEADER_SIZE];
        size_t read_len = 0u;
        uint64_t response_session = 0u;
        uint64_t now_ms = 0u;
        (void)h2_pal_time_get_monotonic_ms(context->runtime->time, &now_ms);
        context->session_id = now_ms ^ ((uint64_t)conn_handle << 48u);
        if (rc == H2_PAL_OK) {
            h2_speed_make_header(
                header, false, context->session_id, H2_SPEED_CHUNK_SIZE,
                H2_PAL_OK);
            rc = h2_bleikcp_write(
                stream, header, sizeof(header), H2_SPEED_SETUP_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK) {
            rc = h2_bleikcp_flush(stream, H2_SPEED_SETUP_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK) {
            rc = h2_bleikcp_read(
                stream, header, sizeof(header), &read_len,
                H2_SPEED_SETUP_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK && read_len == sizeof(header)) {
            rc = h2_speed_validate_header(header, true, &response_session);
            if (rc == H2_PAL_OK && response_session != context->session_id) {
                rc = H2_PAL_ERR_FORMAT;
            }
        } else if (rc == H2_PAL_OK) {
            rc = H2_PAL_ERR_FORMAT;
        }
        if (rc == H2_PAL_OK) {
            if (atomic_load_explicit(
                    &context->connections, memory_order_acquire) > 0u) {
                (void)atomic_fetch_add_explicit(
                    &context->reconnects, 1u, memory_order_acq_rel);
            }
            (void)atomic_fetch_add_explicit(
                &context->connections, 1u, memory_order_acq_rel);
            backoff_ms = 250u;
            h2_speed_log_state(context, "running", "handshake", H2_PAL_OK);
            rc = h2_speed_transfer(context, stream, 0u);
        } else {
            h2_speed_set_error(context, "stream", rc);
            h2_speed_log_state(context, "retry", "handshake", rc);
        }
        if (stream != NULL) {
            (void)h2_bleikcp_close(stream);
        }
        (void)h2_pal_ble_disconnect(context->runtime->ble_host, conn_handle);
        if (management_advertising_paused) {
            int resume_rc = context->resume_management_advertising(
                context->management_advertising_user);
            if (resume_rc != H2_PAL_OK) {
                h2_speed_log_state(
                    context, "warning", "advertising", resume_rc);
            }
        }
        (void)atomic_fetch_add_explicit(
            &context->disconnects, 1u, memory_order_acq_rel);
        h2_speed_log_state(context, "disconnected", "transfer", rc);
        (void)h2_speed_sleep_interruptible(context, backoff_ms);
    }
    return H2_PAL_OK;
}

int h2_bleikcp_speed_run(
    h2_runtime_t *runtime,
    const h2_bleikcp_speed_config_t *config) {
    if (runtime == NULL || runtime->ble_host == NULL || runtime->task == NULL ||
        runtime->time == NULL || runtime->sync == NULL ||
        runtime->system_event == NULL || runtime->mem == NULL ||
        runtime->log == NULL || runtime->display == NULL ||
        config == NULL || config->ready == NULL ||
        (config->role != H2_BLEIKCP_SPEED_ROLE_SERVER &&
         config->role != H2_BLEIKCP_SPEED_ROLE_CLIENT) ||
        (config->advertising_type != H2_PAL_BLE_ADV_TYPE_LEGACY &&
         config->advertising_type != H2_PAL_BLE_ADV_TYPE_EXTENDED) ||
        (config->scan_type != H2_PAL_BLE_SCAN_TYPE_LEGACY &&
         config->scan_type != H2_PAL_BLE_SCAN_TYPE_EXTENDED) ||
        (config->role == H2_BLEIKCP_SPEED_ROLE_CLIENT &&
         (config->pause_management_advertising == NULL ||
          config->resume_management_advertising == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_speed_context_t *context = h2_pal_mem_alloc(runtime->mem, sizeof(*context));
    if (context == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(context, 0, sizeof(*context));
    context->runtime = runtime;
    context->role = config->role;
    context->advertising_type = config->advertising_type;
    context->scan_type = config->scan_type;
    context->pause_management_advertising =
        config->pause_management_advertising;
    context->resume_management_advertising =
        config->resume_management_advertising;
    context->advertise_server_service = config->advertise_server_service;
    context->management_advertising_user =
        config->management_advertising_user;
    context->ready = config->ready;
    context->ready_user = config->ready_user;
    context->should_stop = config->should_stop;
    context->stop_user = config->stop_user;
    context->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    context->last_error_stage = "none";
    (void)snprintf(context->state, sizeof(context->state), "booting");
    h2_pal_mutex_config_t mutex_config = {
        .name = "bleikcp-speed/metrics",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    int rc = h2_pal_mutex_create(
        runtime->sync, &mutex_config, &context->metrics_mutex);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(runtime->mem, context);
        return rc;
    }
    rc = h2_speed_start_display(context);
    if (rc != H2_PAL_OK) {
        return h2_speed_cleanup_error(context, rc);
    }
    rc = h2_pal_ble_start(runtime->ble_host);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_INVALID_STATE) {
        return h2_speed_cleanup_error(context, rc);
    }
    rc = h2_pal_system_event_subscribe(
        runtime->system_event,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
        h2_speed_connection_system_event,
        context,
        &context->connection_subscription);
    if (rc != H2_PAL_OK) {
        return h2_speed_cleanup_error(context, rc);
    }
    if (config->role == H2_BLEIKCP_SPEED_ROLE_CLIENT) {
        rc = h2_speed_start_renderer(context);
        if (rc != H2_PAL_OK) {
            return h2_speed_cleanup_error(context, rc);
        }
        rc = h2_speed_run_client(context);
        return h2_speed_cleanup_error(context, rc);
    }
    rc = h2_speed_start_renderer(context);
    if (rc != H2_PAL_OK) {
        return h2_speed_cleanup_error(context, rc);
    }
    rc = h2_speed_mark_ready(context);
    if (rc != H2_PAL_OK) {
        return h2_speed_cleanup_error(context, rc);
    }
    h2_bleikcp_api_t api = h2_speed_api(runtime);
    h2_bleikcp_config_t stream_config = h2_speed_config(context);
    rc = h2_bleikcp_server_open(
        &api, &stream_config, h2_speed_server_handler, context,
        &context->server);
    if (rc != H2_PAL_OK) {
        h2_speed_preserve_server_error(context, "server", rc);
        return h2_speed_wait_for_stop(context);
    }
    rc = h2_speed_server_start_advertising(context);
    if (rc != H2_PAL_OK) {
        h2_speed_preserve_server_error(context, "advertising", rc);
        return h2_speed_wait_for_stop(context);
    }
    h2_speed_log_state(context, "advertising", "advertising", H2_PAL_OK);
    return h2_speed_wait_for_stop(context);
}
