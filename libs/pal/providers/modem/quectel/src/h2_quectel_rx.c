#include "h2_quectel_internal.h"

#include <stdlib.h>
#include <string.h>

static int command_is(const char *command, const char *expected) {
    return command != NULL && strcmp(command, expected) == 0;
}

int h2_quectel_is_urc(const char *line, const char *command) {
    if (line == NULL || line[0] == '\0') {
        return 0;
    }
    if (strcmp(line, "RING") == 0 || strcmp(line, "NO CARRIER") == 0 ||
        strcmp(line, "BUSY") == 0 || strcmp(line, "NO ANSWER") == 0 ||
        strcmp(line, "RDY") == 0 || strcmp(line, "APP RDY") == 0 ||
        strncmp(line, "+CRING:", 7u) == 0 || strncmp(line, "+CLIP:", 6u) == 0) {
        return 1;
    }
    if (strncmp(line, "+CEREG:", 7u) == 0 || strncmp(line, "+CREG:", 6u) == 0 ||
        strncmp(line, "+CGREG:", 7u) == 0) {
        const char *start = strchr(line, ':') + 1;
        char *end = NULL;
        long stat = strtol(start, &end, 10);
        if (start == end || stat < 0 || stat > 5) { return 0; }
        while (*end == ' ') { end++; }
        if (*end == '\0') { return 1; }
        if (*end++ != ',') { return 0; }
        while (*end == ' ') { end++; }
        /* Query <n>,<stat> differs from URC <stat>,"lac",... even during
         * a registration query. Never discard every matching prefix. */
        return *end == '"';
    }
    if (strncmp(line, "+QSIMSTAT", 9u) == 0) {
        return !command_is(command, "AT+QSIMSTAT?");
    }
    if (strncmp(line, "+CPIN:", 6u) == 0) {
        return !command_is(command, "AT+CPIN?");
    }
    if (strncmp(line, "+CGATT:", 7u) == 0) {
        return !command_is(command, "AT+CGATT?");
    }
    if (strncmp(line, "+CSQ:", 5u) == 0) {
        return !command_is(command, "AT+CSQ");
    }
    if (strncmp(line, "+CLCC:", 6u) == 0) {
        return !command_is(command, "AT+CLCC");
    }
    /* Echoes, unprefixed identity, terminal results and unsupported reports
     * never occupy the notification queue. CME SIM errors are handled by the
     * solicited response path. No global content deduplication is used. */
    return 0;
}

typedef struct rx_context {
    h2_quectel_modem_t *modem;
    const char *command;
} rx_context_t;

static h2_pal_result_t receive_line(void *user, const char *line) {
    rx_context_t *context = user;
    return h2_quectel_is_urc(line, context->command)
        ? h2_quectel_post_urc_line(context->modem, line) : H2_PAL_OK;
}

h2_pal_result_t h2_quectel_rx_feed(
    h2_quectel_modem_t *modem,
    h2_modem_rx_t *receiver,
    uint64_t offset,
    const uint8_t *data,
    size_t length,
    const char *command) {
    if (modem == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    rx_context_t context = {.modem = modem, .command = command};
    return h2_modem_rx_feed(receiver, offset, data, length, receive_line, &context);
}
