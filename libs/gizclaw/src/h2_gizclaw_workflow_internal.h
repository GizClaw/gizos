#ifndef H2_GIZCLAW_WORKFLOW_INTERNAL_H
#define H2_GIZCLAW_WORKFLOW_INTERNAL_H

#include "h2_gizclaw_workflow.h"
#include <stdint.h>
#include <string.h>

/* Tags are opaque UTF-8 spans. No trimming, case folding or category parsing.
 */
static inline bool
h2_gizclaw_workflow_tag_valid_internal(h2_gizclaw_str_t tag) {
  if (!tag.data || !tag.len || tag.len > H2_GIZCLAW_WORKFLOW_TAG_MAX_BYTES ||
      memchr(tag.data, 0, tag.len))
    return false;
  const uint8_t *bytes = (const uint8_t *)tag.data;
  for (size_t i = 0; i < tag.len;) {
    uint32_t code = bytes[i++], minimum = 0;
    size_t rest = 0;
    if (code < 0x80)
      continue;
    if (code >= 0xc2 && code <= 0xdf) {
      code &= 0x1f;
      rest = 1;
      minimum = 0x80;
    } else if (code >= 0xe0 && code <= 0xef) {
      code &= 0x0f;
      rest = 2;
      minimum = 0x800;
    } else if (code >= 0xf0 && code <= 0xf4) {
      code &= 7;
      rest = 3;
      minimum = 0x10000;
    } else
      return false;
    if (tag.len - i < rest)
      return false;
    while (rest--) {
      if ((bytes[i] & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (bytes[i++] & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
static inline bool
h2_gizclaw_workflow_tags_valid_internal(const h2_gizclaw_str_t *tags,
                                        size_t count) {
  if (count > H2_GIZCLAW_WORKFLOW_TAG_MAX_ITEMS || (count && !tags))
    return false;
  for (size_t i = 0; i < count; ++i)
    if (!h2_gizclaw_workflow_tag_valid_internal(tags[i]))
      return false;
  return true;
}
static inline bool
h2_gizclaw_workflow_matches_tags_internal(const h2_gizclaw_workflow_t *workflow,
                                          const h2_gizclaw_str_t *tags,
                                          size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bool found = false;
    for (size_t j = 0; j < workflow->tag_count; ++j)
      if (strlen(workflow->tags[j]) == tags[i].len &&
          !memcmp(workflow->tags[j], tags[i].data, tags[i].len)) {
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  return true;
}
#endif
