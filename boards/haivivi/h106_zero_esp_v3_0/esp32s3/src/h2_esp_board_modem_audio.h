#ifndef H2_ESP_BOARD_MODEM_AUDIO_H
#define H2_ESP_BOARD_MODEM_AUDIO_H

#include <stdio.h>
#include <string.h>

typedef struct {
  int (*at)(void* user, const char* cmd, char* response, unsigned size);
  void (*log)(void* user, const char* line);
  void* user;
} h2_zero_esp_modem_audio_ops_t;

typedef struct {
  unsigned char index;
  unsigned short value;
} h2_zero_esp_modem_audio_eec_t;

typedef struct {
  int mode;
  int sidetone;
  int mic_txgain;
  int mic_txdgain;
  const h2_zero_esp_modem_audio_eec_t* eec;
  unsigned eec_count;
} h2_zero_esp_modem_audio_profile_t;

/* Accept one integer, or two for QMIC, from a complete response line. */
static inline int h2_zero_esp_modem_audio_parse(const char* response,
                                                const char* prefix, int mic,
                                                char* value) {
  const char* p = response;
  while ((p = strstr(p, prefix)) != NULL) {
    if (p == response || p[-1] == '\r' || p[-1] == '\n') break;
    ++p;
  }
  if (p == NULL) return 0;
  p += strlen(prefix);
  unsigned used = 0;
  for (unsigned field = 0; field < 2u; ++field) {
    while (*p == ' ' || *p == '\t') ++p;
    unsigned digits = 0, number = 0;
    while (*p >= '0' && *p <= '9') {
      if (++digits > 5u) return 0;
      number = number * 10u + (unsigned)(*p++ - '0');
    }
    if (digits == 0u || number > 65535u) return 0;
    used += (unsigned)snprintf(value + used, 16u - used, "%u", number);
    while (*p == ' ' || *p == '\t') ++p;
    if (mic && field == 0u && *p == ',') {
      value[used++] = ',';
      ++p;
      continue;
    }
    return *p == '\0' || *p == '\r' || *p == '\n';
  }
  return 0;
}

static inline unsigned h2_zero_esp_modem_audio_snapshot(
    const h2_zero_esp_modem_audio_ops_t* ops, const char* stage) {
  const char* commands[] = {"AT+QAUDMOD?", "AT+QSIDET?", "AT+QMIC?",
                            "AT+CLVL?"};
  const char* prefixes[] = {"+QAUDMOD:", "+QSIDET:", "+QMIC:", "+CLVL:"};
  char values[4][16];
  unsigned failures = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    char response[128] = {0};
    const int rc = ops->at(ops->user, commands[i], response, sizeof(response));
    response[sizeof(response) - 1u] = '\0';
    failures += rc != 0;
    if (rc != 0 || !h2_zero_esp_modem_audio_parse(response, prefixes[i],
                                                  i == 2u, values[i]))
      strcpy(values[i], "?");
  }
  char line[160];
  snprintf(line, sizeof(line),
           "H2_MODEM_AUDIO stage=%s qaudmod=%s qsidet=%s qmic=%s clvl=%s",
           stage, values[0], values[1], values[2], values[3]);
  ops->log(ops->user, line);
  return failures;
}

static inline unsigned h2_zero_esp_modem_audio_write(
    const h2_zero_esp_modem_audio_ops_t* ops, const char* cmd) {
  char response[128] = {0}, line[128];
  const int rc = ops->at(ops->user, cmd, response, sizeof(response));
  snprintf(line, sizeof(line), "H2_MODEM_AUDIO apply cmd=%s rc=%d", cmd, rc);
  ops->log(ops->user, line);
  return rc != 0;
}

/* Count failed AT commands without interrupting the remaining profile. */
static inline unsigned h2_zero_esp_modem_audio_apply(
    const h2_zero_esp_modem_audio_ops_t* ops,
    const h2_zero_esp_modem_audio_profile_t* profile) {
  unsigned failures = h2_zero_esp_modem_audio_snapshot(ops, "before");
  char cmd[64];
  if (profile->mode >= 0) {
    snprintf(cmd, sizeof(cmd), "AT+QAUDMOD=%d", profile->mode);
    failures += h2_zero_esp_modem_audio_write(ops, cmd);
  }
  failures += h2_zero_esp_modem_audio_snapshot(ops, "after");
  if (profile->sidetone >= 0) {
    snprintf(cmd, sizeof(cmd), "AT+QSIDET=%d", profile->sidetone);
    failures += h2_zero_esp_modem_audio_write(ops, cmd);
  }
  if (profile->mic_txgain >= 0) {
    if (profile->mic_txdgain >= 0)
      snprintf(cmd, sizeof(cmd), "AT+QMIC=%d,%d", profile->mic_txgain,
               profile->mic_txdgain);
    else
      snprintf(cmd, sizeof(cmd), "AT+QMIC=%d", profile->mic_txgain);
    failures += h2_zero_esp_modem_audio_write(ops, cmd);
  }
  for (unsigned i = 0; profile->eec != NULL && i < profile->eec_count; ++i) {
    snprintf(cmd, sizeof(cmd), "AT+QEEC=%u,%u", (unsigned)profile->eec[i].index,
             (unsigned)profile->eec[i].value);
    failures += h2_zero_esp_modem_audio_write(ops, cmd);
  }
  ops->log(ops->user,
           "H2_MODEM_AUDIO qeec_dump=skipped reason=exceeds_128_byte_response");
  return failures;
}
#endif
