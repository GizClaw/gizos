#include "h2_esp_board_modem_audio.h"
#include <assert.h>
#include <string.h>

typedef struct {
  char commands[16][64];
  char logs[16][160];
  unsigned count, log_count, failures;
  const char* responses[4];
} fixture_t;

static int at(void* user, const char* cmd, char* response, unsigned size) {
  fixture_t* f = user;
  const unsigned index = f->count++;
  assert(index < 16u);
  snprintf(f->commands[index], sizeof(f->commands[index]), "%s", cmd);
  const char* reply = "OK";
  if (strcmp(cmd, "AT+QAUDMOD?") == 0)
    reply = f->responses[0] ? f->responses[0]
                            : (index == 0u ? "+QAUDMOD: 0" : "+QAUDMOD: 2");
  if (strcmp(cmd, "AT+QSIDET?") == 0)
    reply = f->responses[1] ? f->responses[1] : "\r\n+QSIDET: 0\r\nOK\r\n";
  if (strcmp(cmd, "AT+QMIC?") == 0)
    reply = f->responses[2] ? f->responses[2]
                            : "AT+QMIC?\r\n+QMIC: 100, 200\r\nOK\r\n";
  if (strcmp(cmd, "AT+CLVL?") == 0)
    reply = f->responses[3] ? f->responses[3] : "+CLVL: 5";
  snprintf(response, size, "%s", reply);
  return (f->failures & (1u << index)) ? -7 : 0;
}
static void log_line(void* user, const char* line) {
  fixture_t* f = user;
  assert(f->log_count < 16u);
  snprintf(f->logs[f->log_count++], sizeof(f->logs[0]), "%s", line);
  if (strstr(line, "stage=before")) assert(f->count == 4u);
  if (strstr(line, "stage=after")) assert(f->count == 8u || f->count == 9u);
}
static const char* sequence[] = {
    "AT+QAUDMOD?",  "AT+QSIDET?",  "AT+QMIC?",        "AT+CLVL?",
    "AT+QAUDMOD=2", "AT+QAUDMOD?", "AT+QSIDET?",      "AT+QMIC?",
    "AT+CLVL?",     "AT+QSIDET=0", "AT+QMIC=123,456", "AT+QEEC=0,65535",
    "AT+QEEC=50,0"};

static void test_profile(void) {
  const h2_tiga_modem_audio_eec_t eec[] = {{0, 65535}, {50, 0}};
  const h2_tiga_modem_audio_profile_t profile = {2, 0, 123, 456, eec, 2};
  for (unsigned failure = 0; failure <= 13u; ++failure) {
    fixture_t f = {0};
    f.failures = failure < 13u ? 1u << failure : 0u;
    const h2_tiga_modem_audio_ops_t ops = {at, log_line, &f};
    assert(h2_tiga_modem_audio_apply(&ops, &profile) ==
           (failure < 13u ? 1u : 0u));
    assert(f.count == 13u && f.log_count == 8u);
    for (unsigned i = 0; i < f.count; ++i)
      assert(strcmp(f.commands[i], sequence[i]) == 0);
    for (unsigned i = 4u, line = 1u; i < f.count; ++i) {
      if (i > 4u && i < 9u) continue;
      if (i == 9u) ++line;
      char expected[128];
      snprintf(expected, sizeof(expected), "H2_MODEM_AUDIO apply cmd=%s rc=%d",
               sequence[i], i == failure ? -7 : 0);
      assert(strcmp(f.logs[line++], expected) == 0);
    }
    if (failure == 13u) {
      assert(strcmp(f.logs[0],
                    "H2_MODEM_AUDIO stage=before qaudmod=0 qsidet=0 "
                    "qmic=100,200 clvl=5") == 0);
      assert(strcmp(f.logs[2],
                    "H2_MODEM_AUDIO stage=after qaudmod=2 qsidet=0 "
                    "qmic=100,200 clvl=5") == 0);
    }
    if (failure == 2u) assert(strstr(f.logs[0], "qmic=?") != NULL);
  }
  fixture_t f = {.failures = (1u << 1) | (1u << 4) | (1u << 10)};
  const h2_tiga_modem_audio_ops_t ops = {at, log_line, &f};
  assert(h2_tiga_modem_audio_apply(&ops, &profile) == 3u);
  assert(f.count == 13u);
}
static void test_optional(void) {
  fixture_t f = {0};
  const h2_tiga_modem_audio_ops_t ops = {at, log_line, &f};
  h2_tiga_modem_audio_profile_t profile = {2, 0, -1, -1, NULL, 0};
  assert(h2_tiga_modem_audio_apply(&ops, &profile) == 0u);
  assert(f.count == 10u);
  for (unsigned i = 0; i < f.count; ++i)
    assert(strcmp(f.commands[i], sequence[i]) == 0);
  f = (fixture_t){0};
  profile = (h2_tiga_modem_audio_profile_t){-1, -1, -1, 10, NULL, 2};
  assert(h2_tiga_modem_audio_apply(&ops, &profile) == 0u);
  assert(f.count == 8u);
  for (unsigned i = 0; i < f.count; ++i)
    assert(strcmp(f.commands[i], sequence[i % 4u]) == 0);
  f = (fixture_t){0};
  const h2_tiga_modem_audio_eec_t eec = {1, 2};
  profile = (h2_tiga_modem_audio_profile_t){2, 0, 123, -1, &eec, 0};
  assert(h2_tiga_modem_audio_apply(&ops, &profile) == 0u);
  assert(f.count == 11u && strcmp(f.commands[10], "AT+QMIC=123") == 0);
}
static void test_parse(void) {
  const char* bad[] = {"",
                       "OK",
                       "+QMIC: 1,",
                       "+QMIC: 1,2,3",
                       "+QMIC: 65536",
                       "+QMIC: 1234567890",
                       "+QMIC: -1",
                       "+QMIC: 2oops",
                       "junk+QMIC: 2"};
  const h2_tiga_modem_audio_profile_t profile = {2, 0, -1, -1, NULL, 0};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    fixture_t f = {
        .responses = {"+QAUDMOD: x", "+QSIDET: 0junk", bad[i], "+CLVL:"}};
    const h2_tiga_modem_audio_ops_t ops = {at, log_line, &f};
    assert(h2_tiga_modem_audio_apply(&ops, &profile) == 0u);
    assert(
        strcmp(
            f.logs[0],
            "H2_MODEM_AUDIO stage=before qaudmod=? qsidet=? qmic=? clvl=?") ==
        0);
    assert(
        strcmp(f.logs[2],
               "H2_MODEM_AUDIO stage=after qaudmod=? qsidet=? qmic=? clvl=?") ==
        0);
    assert(f.count == 10u);
  }
  fixture_t f = {.responses = {NULL, NULL, "+QMIC: 65535\r\nOK", NULL}};
  const h2_tiga_modem_audio_ops_t ops = {at, log_line, &f};
  assert(h2_tiga_modem_audio_apply(&ops, &profile) == 0u);
  assert(strstr(f.logs[2], "qmic=65535 clvl=5") != NULL);
}
int main(void) {
  test_profile();
  test_optional();
  test_parse();
  return 0;
}
