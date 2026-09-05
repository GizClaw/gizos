#include "h2_iperf_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>

static void test_writer(void) {
    char buf[256];
    h2_iperf_json_writer_t w;
    h2_iperf_json_init(&w, buf, sizeof(buf));
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "tcp");
    h2_iperf_json_bool(&w, true);
    h2_iperf_json_key(&w, "time");
    h2_iperf_json_i64(&w, -3);
    h2_iperf_json_key(&w, "num");
    h2_iperf_json_u64(&w, 18446744073709551615ull);
    h2_iperf_json_key(&w, "jitter");
    h2_iperf_json_f64(&w, 0.0001775);
    h2_iperf_json_key(&w, "title");
    h2_iperf_json_string(&w, "a\"b\\c\n");
    h2_iperf_json_key(&w, "streams");
    h2_iperf_json_array_begin(&w);
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "id");
    h2_iperf_json_i64(&w, 1);
    h2_iperf_json_object_end(&w);
    h2_iperf_json_i64(&w, 2);
    h2_iperf_json_array_end(&w);
    h2_iperf_json_object_end(&w);
    assert(h2_iperf_json_finish(&w));
    assert(strcmp(buf,
                  "{\"tcp\":true,\"time\":-3,\"num\":18446744073709551615,"
                  "\"jitter\":0.000178,\"title\":\"a\\\"b\\\\c\\n\","
                  "\"streams\":[{\"id\":1},2]}") == 0);

    char small[8];
    h2_iperf_json_init(&w, small, sizeof(small));
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "overflow");
    h2_iperf_json_i64(&w, 1);
    h2_iperf_json_object_end(&w);
    assert(!h2_iperf_json_finish(&w));

    h2_iperf_json_init(&w, buf, sizeof(buf));
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "dangling");
    assert(!h2_iperf_json_finish(&w));
}

static void test_reader(void) {
    /* Shaped like a real iperf3 3.21 server result including nested output. */
    const char *json =
        "{\"cpu_util_total\":1.5,\"cpu_util_user\":0.25,\"cpu_util_system\":1.25,"
        "\"sender_has_retransmits\":-1,\"congestion_used\":\"cubic\","
        "\"server_output_text\":\"bytes: \\\"fake\\\" 99\","
        "\"streams\":[{\"id\":1,\"bytes\":1310720,\"retransmits\":-1,"
        "\"jitter\":0.000177,\"errors\":2,\"omitted_errors\":0,\"packets\":16,"
        "\"omitted_packets\":0,\"start_time\":0,\"end_time\":1.000123}]}";
    size_t len = strlen(json);
    double number = 0.0;
    int64_t integer = 0;
    char text[32];
    assert(h2_iperf_json_get_f64(json, len, "cpu_util_total", &number));
    assert(fabs(number - 1.5) < 1e-9);
    assert(h2_iperf_json_get_i64(json, len, "sender_has_retransmits", &integer));
    assert(integer == -1);
    assert(h2_iperf_json_get_string(json, len, "congestion_used", text, sizeof(text)));
    assert(strcmp(text, "cubic") == 0);
    assert(h2_iperf_json_get_string(json, len, "server_output_text", text, sizeof(text)));
    assert(strcmp(text, "bytes: \"fake\" 99") == 0);
    assert(!h2_iperf_json_get_string(json, len, "missing", text, sizeof(text)));
    assert(!h2_iperf_json_get_true(json, len, "cpu_util_total"));

    h2_iperf_stream_stats_t stats;
    assert(h2_iperf_parse_results_json(json, len, &stats));
    assert(stats.bytes == 1310720u);
    assert(stats.packets == 16u);
    assert(stats.lost_packets == 2);
    assert(stats.retransmits == -1);
    assert(fabs(stats.jitter_ms - 0.177) < 1e-6);
    assert(stats.duration_ms == 1000u);

    const char *params =
        "{\"udp\":true,\"omit\":0,\"time\":10,\"num\":0,\"blockcount\":0,"
        "\"parallel\":1,\"reverse\":true,\"len\":1460,\"bandwidth\":1048576,"
        "\"pacing_timer\":1000,\"client_version\":\"3.21\"}";
    len = strlen(params);
    assert(h2_iperf_json_get_true(params, len, "udp"));
    assert(!h2_iperf_json_get_true(params, len, "tcp"));
    assert(h2_iperf_json_get_true(params, len, "reverse"));
    assert(h2_iperf_json_get_i64(params, len, "bandwidth", &integer));
    assert(integer == 1048576);
    assert(h2_iperf_json_get_i64(params, len, "len", &integer));
    assert(integer == 1460);

    assert(h2_iperf_json_parse_f64("1e3", 3u, &number) && fabs(number - 1000.0) < 1e-9);
    assert(h2_iperf_json_parse_f64("-2.5E-1", 7u, &number) && fabs(number + 0.25) < 1e-12);
    assert(!h2_iperf_json_parse_f64("abc", 3u, &number));
    assert(!h2_iperf_json_parse_f64("1.2.3", 5u, &number));
    assert(!h2_iperf_parse_results_json("{\"streams\":[]}", 14u, &stats));
}

static void test_results_roundtrip(void) {
    char buf[512];
    h2_iperf_stream_stats_t in;
    memset(&in, 0, sizeof(in));
    in.bytes = 123456789u;
    in.packets = 4242u;
    in.lost_packets = 7;
    in.jitter_ms = 1.25;
    in.duration_ms = 2500u;
    assert(h2_iperf_build_results_json(buf, sizeof(buf), false, &in));
    h2_iperf_stream_stats_t out;
    assert(h2_iperf_parse_results_json(buf, strlen(buf), &out));
    assert(out.bytes == in.bytes);
    assert(out.packets == in.packets);
    assert(out.lost_packets == 7);
    assert(fabs(out.jitter_ms - 1.25) < 1e-6);
    assert(out.duration_ms == 2500u);
    assert(strstr(buf, "\"sender_has_retransmits\":-1") != NULL);
    assert(h2_iperf_build_results_json(buf, sizeof(buf), true, &in));
    assert(strstr(buf, "\"sender_has_retransmits\":0") != NULL);
    assert(!h2_iperf_build_results_json(buf, 16u, true, &in));
}

int main(void) {
    test_writer();
    test_reader();
    test_results_roundtrip();
    return 0;
}
