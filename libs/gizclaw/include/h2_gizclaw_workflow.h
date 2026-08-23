#ifndef H2_GIZCLAW_WORKFLOW_H
#define H2_GIZCLAW_WORKFLOW_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES 63u
#define H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES 63u
#define H2_GIZCLAW_WORKFLOW_LOCALE_MAX_BYTES 31u
#define H2_GIZCLAW_WORKFLOW_DISPLAY_NAME_MAX_BYTES 127u
#define H2_GIZCLAW_WORKFLOW_DESCRIPTION_MAX_BYTES 255u
#define H2_GIZCLAW_WORKFLOW_LANG_PAIR_MAX_BYTES 31u
#define H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS 64u
#define H2_GIZCLAW_WORKFLOW_I18N_MAX_ITEMS 8u

typedef enum h2_gizclaw_workflow_driver {
  H2_GIZCLAW_WORKFLOW_DRIVER_UNSPECIFIED = 0,
  H2_GIZCLAW_WORKFLOW_DRIVER_FLOWCRAFT = 1,
  H2_GIZCLAW_WORKFLOW_DRIVER_DOUBAO_REALTIME = 2,
  H2_GIZCLAW_WORKFLOW_DRIVER_AST_TRANSLATE = 3,
  H2_GIZCLAW_WORKFLOW_DRIVER_CHATROOM = 4,
  H2_GIZCLAW_WORKFLOW_DRIVER_PET = 5,
} h2_gizclaw_workflow_driver_t;

typedef struct h2_gizclaw_workflow_i18n {
  char *locale;
  char *display_name;
  char *description;
} h2_gizclaw_workflow_i18n_t;

/** Owned safe projection of one Runtime Profile workflow. */
typedef struct h2_gizclaw_workflow {
  char *collection;
  char *name;
  h2_gizclaw_workflow_driver_t driver;
  h2_gizclaw_workflow_i18n_t *i18n;
  size_t i18n_count;
  char *workspace_lang_pair;
} h2_gizclaw_workflow_t;

/** Owned page. Release it with h2_gizclaw_workflow_page_deinit(). */
typedef struct h2_gizclaw_workflow_page {
  h2_gizclaw_workflow_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workflow_page_t;

int h2_gizclaw_client_workflows_list(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t collection,
                                     h2_gizclaw_str_t cursor, size_t limit,
                                     h2_gizclaw_workflow_page_t *out_page);

int h2_gizclaw_client_workflow_get(h2_gizclaw_client_t *client,
                                   h2_gizclaw_str_t name,
                                   h2_gizclaw_workflow_t *out_workflow,
                                   char **out_runtime_profile_name,
                                   char **out_runtime_profile_revision);

const char *
h2_gizclaw_workflow_display_name(const h2_gizclaw_workflow_t *workflow,
                                 const char *locale);
const char *
h2_gizclaw_workflow_description(const h2_gizclaw_workflow_t *workflow,
                                const char *locale);

void h2_gizclaw_workflow_deinit(h2_gizclaw_client_t *client,
                                h2_gizclaw_workflow_t *workflow);
void h2_gizclaw_workflow_get_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_workflow_t *workflow,
                                    char *runtime_profile_name,
                                    char *runtime_profile_revision);
void h2_gizclaw_workflow_page_deinit(h2_gizclaw_client_t *client,
                                     h2_gizclaw_workflow_page_t *page);

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_workflow_decode_list_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workflow_page_t *out_page);
#endif

#ifdef __cplusplus
}
#endif

#endif
