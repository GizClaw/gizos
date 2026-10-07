#include "h2_pal_ipv6_runner.h"
#include <stdio.h>
#include <string.h>
static void item_json(FILE *file, const h2_net_tls_case_result_t *item) {
  fprintf(file,
          "{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,"
          "\"elapsed_ms\":%llu,\"bytes_sent\":%zu,\"bytes_received\":%zu}",
          item->id ? item->id : "missing",
          item->passed    ? "PASS"
          : item->blocked ? "BLOCKED"
                          : "FAIL",
          item->detail, item->line, (unsigned long long)item->elapsed_ms,
          item->bytes_sent, item->bytes_received);
}
void h2_ipv6_report(void *user, const h2_net_tls_case_result_t *item) {
  FILE *file = user ? user : stdout;
  fputs("H2_PAL_IPV6_CASE ", file);
  item_json(file, item);
  fputc('\n', file);
  fflush(file);
}
int h2_ipv6_write_report(const char *path, const char *platform,
                         const h2_pal_ipv6_result_t *result, int rc,
                         int cleanup) {
  FILE *file = path ? fopen(path, "w") : stdout;
  if (!file)
    return H2_PAL_ERR_IO;
  if (!path)
    fputs("H2_PAL_IPV6_SUMMARY ", file);
  fprintf(file,
          "{\"platform\":\"%s\",\"profile\":\"pal-ipv6-native\",\"passed\":%u,"
          "\"failed\":%u,\"blocked\":%u,\"retained_sockets\":%zu,\"retained_"
          "resolvers\":%zu,"
          "\"retained_allocations\":%zu,\"retained_tasks\":%zu,"
          "\"cleanup_error\":%d,\"rc\":%d,\"teardown\":%d,\"cases\":[",
          platform, result->passed, result->failed, result->blocked,
          result->retained_sockets, result->retained_resolvers,
          result->retained_allocations, result->retained_tasks,
          result->cleanup_error, rc, cleanup);
  for (unsigned i = 0u; i < H2_PAL_IPV6_CASES; ++i) {
    if (i)
      fputc(',', file);
    item_json(file, &result->cases[i]);
  }
  fputs("]}\n", file);
  int error = ferror(file);
  if (path && fclose(file))
    error = 1;
  else if (!path)
    fflush(file);
  return error ? H2_PAL_ERR_IO : H2_PAL_OK;
}
