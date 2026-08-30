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

static const char *role_name(h2_h2loader_host_active_role_t role) {
  return role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP ? "app" :
      role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ? "loader" : "unknown";
}

static const char *boot_intent_name(h2_h2loader_host_boot_intent_t intent) {
  return intent == H2_H2LOADER_HOST_BOOT_INTENT_AUTO ? "auto" :
      intent == H2_H2LOADER_HOST_BOOT_INTENT_LOADER ? "loader" : "unknown";
}

static void append_metadata(
    h2_web_json_writer_t *writer,
    const h2_h2loader_host_metadata_t *metadata) {
  append_format(writer, "{\"valid\":%s,\"packageChecksum\":",
                metadata->valid ? "true" : "false");
  append_json_string(writer, metadata->package_checksum);
  append_format(writer, ",\"packageSize\":\"%llu\",\"imageChecksum\":",
                (unsigned long long)metadata->package_size);
  append_json_string(writer, metadata->image_checksum);
  append_format(writer, ",\"imageSize\":\"%llu\",\"role\":",
                (unsigned long long)metadata->image_size);
  append_json_string(writer, role_name(metadata->role));
  append_text(writer, ",\"version\":");
  append_json_string(writer, metadata->version);
  append_text(writer, ",\"board\":");
  append_json_string(writer, metadata->board);
  append_text(writer, ",\"target\":");
  append_json_string(writer, metadata->target);
  append_text(writer, "}");
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
  append_format(&writer,
                ",\"capabilities\":%u,\"commandAvailability\":%u",
                (unsigned int)status->capabilities,
                (unsigned int)status->command_availability);
  append_text(&writer, ",\"active\":{\"role\":");
  append_json_string(&writer, role_name(
      h2_h2loader_host_status_active_role(status)));
  append_text(&writer, ",\"version\":");
  append_json_string(&writer, status->active_version);
  append_text(&writer, ",\"imageChecksum\":");
  append_json_string(&writer, status->active_checksum);
  append_format(&writer,
                "},\"runningPartition\":%u,\"nextPartition\":%u,"
                "\"bootIntent\":",
                (unsigned)status->running_partition,
                (unsigned)status->next_partition);
  append_json_string(&writer, boot_intent_name(
      (h2_h2loader_host_boot_intent_t)
          h2_h2loader_host_status_boot_intent(status)));
  append_text(&writer, ",\"stage\":");
  append_metadata(&writer, &status->stage);
  append_text(&writer, ",\"partition1\":");
  append_metadata(&writer, &status->partition_1);
  append_text(&writer, ",\"partition2\":");
  append_metadata(&writer, &status->partition_2);
  append_format(&writer, ",\"lastResult\":%d,\"mfg\":{\"mode\":%u,\"steps\":[",
                (int)status->last, (unsigned)status->mfg_mode);
  for (size_t index = 0u; index < H2_H2LOADER_HOST_MFG_STEP_TOTAL; ++index) {
    append_format(&writer, "%s%u", index == 0u ? "" : ",",
                  (unsigned)status->mfg_steps[index]);
  }
  append_text(&writer, "]}");
  append_text(&writer, "}");

  if (writer.failed) {
    out[0] = '\0';
    return H2_PAL_ERR_NO_SPACE;
  }
  *out_size = writer.offset;
  return H2_PAL_OK;
}
