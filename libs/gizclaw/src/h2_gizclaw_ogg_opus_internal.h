#ifndef H2_GIZCLAW_OGG_OPUS_INTERNAL_H
#define H2_GIZCLAW_OGG_OPUS_INTERNAL_H

/* GizClaw keeps its internal names; the parser/decoder has one public owner. */
#include "h2_ogg_opus.h"
typedef h2_ogg_opus_t h2_gizclaw_ogg_opus_t;
typedef h2_ogg_opus_read_fn h2_gizclaw_ogg_opus_read_fn;
#define H2_GIZCLAW_OGG_OPUS_PCM_BYTES H2_OGG_OPUS_PCM_BYTES
#define h2_gizclaw_ogg_opus_create h2_ogg_opus_create
#define h2_gizclaw_ogg_opus_create_reader h2_ogg_opus_create_reader
#define h2_gizclaw_ogg_opus_next h2_ogg_opus_next
#define h2_gizclaw_ogg_opus_headers_done h2_ogg_opus_headers_done
#define h2_gizclaw_ogg_opus_seek h2_ogg_opus_seek
#define h2_gizclaw_ogg_opus_origin h2_ogg_opus_origin
#define h2_gizclaw_ogg_opus_destroy h2_ogg_opus_destroy

#endif
