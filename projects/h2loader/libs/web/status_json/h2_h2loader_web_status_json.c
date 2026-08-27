#include "h2_h2loader_web_status_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct h2_web_json_writer {
  char *out;
  size_t capacity;
  size_t offset;
  int failed;
} h2_web_json_writer_t;

static void append_text(h2_web_json_writer_t *writer, const char *text) {
  if (writer->failed) return;
  const size_t len = strlen(text);
  if (writer->offset >= writer->capacity ||
      len >= writer->capacity - writer->offset) {
    writer->failed = 1;
    return;
  }
  memcpy(writer->out + writer->offset, text, len + 1u);
  writer->offset += len;
}

static void append_format(h2_web_json_writer_t *writer, const char *format,
                          ...) {
  if (writer->failed || writer->offset >= writer->capacity) return;
  va_list arguments;
  va_start(arguments, format);
  const int count = vsnprintf(writer->out + writer->offset,
                              writer->capacity - writer->offset, format,
                              arguments);
  va_end(arguments);
  if (count < 0 || (size_t)count >= writer->capacity - writer->offset) {
    writer->failed = 1;
    return;
  }
  writer->offset += (size_t)count;
}

static void append_json_string(h2_web_json_writer_t *writer,
                               const char *text) {
  append_text(writer, "\"");
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor != '\0' && !writer->failed; ++cursor) {
    if (*cursor == '"' || *cursor == '\\') {
      char escaped[3];
      escaped[0] = '\\';
      escaped[1] = (char)*cursor;
      escaped[2] = '\0';
      append_text(writer, escaped);
    } else if (*cursor < 0x20u) {
      append_format(writer, "\\u%04x", (unsigned int)*cursor);
    } else {
      char escaped[2];
      escaped[0] = (char)*cursor;
      escaped[1] = '\0';
      append_text(writer, escaped);
    }
  }
  append_text(writer, "\"");
}

h2_pal_result_t h2_h2loader_web_status_json_write(
    const h2_h2loader_host_status_t *status, char *out, size_t capacity,
    size_t *out_size) {
  if (out_size == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_size = 0u;
  if (status == NULL || out == NULL || capacity == 0u) {
    if (out != NULL && capacity > 0u) out[0] = '\0';
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_web_json_writer_t writer = {
      .out = out,
      .capacity = capacity,
  };
  out[0] = '\0';
  append_text(&writer, "{\"board\":");
  append_json_string(&writer, status->board);
  append_text(&writer, ",\"target\":");
  append_json_string(&writer, status->target);
  append_text(&writer, ",\"chip\":");
  append_json_string(&writer, status->chip);
  append_text(&writer, ",\"activeName\":");
  append_json_string(&writer, status->active_name);
  append_text(&writer, ",\"version\":");
  append_json_string(&writer, status->active_version);
  append_text(&writer, ",\"checksum\":");
  append_json_string(&writer, status->active_checksum);
  append_text(&writer, ",\"installedChecksum\":");
  append_json_string(&writer, status->installed_checksum);
  append_text(&writer, ",\"state\":");
  append_json_string(&writer, status->state);
  append_text(&writer, ",\"upgradePhase\":");
  append_json_string(&writer, status->upgrade_phase);

  append_format(&writer, ",\"role\":%u,\"capabilities\":%u",
                (unsigned int)status->active_role,
                (unsigned int)status->capabilities);
  if (status->has_command_availability) {
    append_format(&writer, ",\"commandAvailability\":%u",
                  (unsigned int)status->command_availability);
  }
  append_format(&writer,
                ",\"stagedBytes\":%llu,\"stagedValid\":%u,"
                "\"installedValid\":%u,\"appConfirmed\":%u}",
                (unsigned long long)status->staged_bytes,
                (unsigned int)status->staged_valid,
                (unsigned int)status->installed_valid,
                (unsigned int)status->app_confirmed);

  if (writer.failed) {
    out[0] = '\0';
    return H2_PAL_ERR_NO_SPACE;
  }
  *out_size = writer.offset;
  return H2_PAL_OK;
}
