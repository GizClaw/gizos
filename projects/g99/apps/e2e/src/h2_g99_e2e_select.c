#include "h2_g99_e2e_internal.h"

#include <string.h>

/* A selection is a comma-separated list of terms: a group ("navigation"), a
 * case ("navigation/library-enter-back") or a prefix ending in '*'. A leading
 * '-' excludes. Without include terms every case is included. */

typedef struct term {
  const char* text;
  size_t length;
  bool exclude;
} term_t;

static bool is_space(char c) { return c == ' ' || c == '\t'; }

static bool next_term(const char** cursor, term_t* out) {
  const char* p = *cursor;
  while (p != NULL && *p != '\0') {
    const char* end = strchr(p, ',');
    if (end == NULL) end = p + strlen(p);
    const char* start = p;
    p = *end == ',' ? end + 1 : end;
    while (start < end && is_space(*start)) ++start;
    while (end > start && is_space(end[-1])) --end;
    const bool exclude = start < end && *start == '-';
    if (exclude) ++start;
    if (start < end) {
      *cursor = p;
      *out = (term_t){start, (size_t)(end - start), exclude};
      return true;
    }
  }
  *cursor = p;
  return false;
}

/* Compares text with "<group>/<name>"; with prefix, text only starts it. */
static bool id_matches(const char* text, size_t length, bool prefix,
                       const char* group, const char* name) {
  const size_t group_length = strlen(group);
  const size_t id_length = group_length + 1u + strlen(name);
  if (prefix ? length > id_length : length != id_length) return false;
  for (size_t i = 0u; i < length; ++i) {
    const char expected = i < group_length    ? group[i]
                          : i == group_length ? '/'
                                              : name[i - group_length - 1u];
    if (text[i] != expected) return false;
  }
  return true;
}

static bool term_matches(const term_t* term, const char* group,
                         const char* name) {
  if (term->text[term->length - 1u] == '*')
    return id_matches(term->text, term->length - 1u, true, group, name);
  if (memchr(term->text, '/', term->length) != NULL)
    return id_matches(term->text, term->length, false, group, name);
  return strlen(group) == term->length &&
         strncmp(group, term->text, term->length) == 0;
}

bool h2_g99_e2e_selected(const char* select, const char* group,
                         const char* name) {
  bool has_include = false;
  bool included = false;
  bool excluded = false;
  term_t term;
  for (const char* cursor = select; next_term(&cursor, &term);) {
    const bool matches = term_matches(&term, group, name);
    if (term.exclude) {
      excluded = excluded || matches;
    } else {
      has_include = true;
      included = included || matches;
    }
  }
  return (included || !has_include) && !excluded;
}

bool h2_g99_e2e_select_valid(const char* select, char* out_term,
                             size_t out_term_size) {
  if (out_term != NULL && out_term_size != 0u) out_term[0] = '\0';
  if (select != NULL && strlen(select) > H2_G99_E2E_SELECT_MAX) {
    if (out_term != NULL && out_term_size != 0u)
      (void)strncpy(out_term, "<too long>", out_term_size - 1u);
    return false;
  }
  term_t term;
  for (const char* cursor = select; next_term(&cursor, &term);) {
    bool known = false;
    for (size_t g = 0u; !known && g < h2_g99_e2e_group_count; ++g) {
      const h2_g99_e2e_group_t* group = h2_g99_e2e_groups[g];
      for (size_t c = 0u; !known && c < group->count; ++c)
        known = term_matches(&term, group->name, group->cases[c].name);
    }
    if (!known) {
      if (out_term != NULL && out_term_size != 0u) {
        const size_t length =
            term.length < out_term_size - 1u ? term.length : out_term_size - 1u;
        memcpy(out_term, term.text, length);
        out_term[length] = '\0';
      }
      return false;
    }
  }
  return true;
}
