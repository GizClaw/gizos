#ifndef H2_YYJSON_JSON_INTERNAL_H
#define H2_YYJSON_JSON_INTERNAL_H

#include "h2_yyjson_json.h"

#include "yyjson.h"

#include <stdbool.h>

typedef struct h2_yyjson_buffer_record h2_yyjson_buffer_record_t;

struct h2_yyjson_json {
    h2_pal_mem_api_t mem;
    h2_pal_json_api_t api;
    yyjson_alc allocator;
    h2_yyjson_buffer_record_t *buffers;
    size_t live_documents;
    size_t live_buffers;
};

struct h2_pal_json_document {
    h2_yyjson_json_t *provider;
    h2_pal_json_limits_t limits;
    h2_pal_json_value_t *values;
    h2_pal_json_value_t *root;
    yyjson_doc *parsed_document;
    yyjson_mut_doc *mutable_document;
    size_t value_handles;
};

struct h2_pal_json_value {
    h2_pal_json_document_t *document;
    h2_pal_json_value_t *parent;
    h2_pal_json_value_t *next;
    union {
        yyjson_val *parsed;
        yyjson_mut_val *mutable_value;
    } node;
    bool mutable_value;
    bool attached;
    bool stale;
};

struct h2_yyjson_buffer_record {
    h2_yyjson_buffer_record_t *next;
    uint8_t *data;
};

#endif
