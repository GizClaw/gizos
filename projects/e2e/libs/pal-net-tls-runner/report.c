#include "runner.h"

static void case_json(FILE *file, const h2_net_tls_case_result_t *item) {
  fprintf(file,
          "{\"id\":\"%s\",\"mandatory\":%s,\"status\":\"%s\",\"detail\":%d,"
          "\"line\":%u,\"elapsed_ms\":%llu,\"bytes_sent\":%zu,\"bytes_"
          "received\":%zu,\"provider_result\":%d}",
          item->id ? item->id : "", item->mandatory ? "true" : "false",
          item->passed         ? "PASS"
          : item->blocked      ? "BLOCKED"
          : item->unsupported  ? "UNSUPPORTED"
          : item->not_assessed ? "NOT_ASSESSED"
                               : "FAIL",
          item->detail, item->line, (unsigned long long)item->elapsed_ms,
          item->bytes_sent, item->bytes_received, item->provider_result);
}
void h2_net_tls_report(void *user, const h2_net_tls_case_result_t *item) {
  FILE *file = user ? user : stdout;
  fputs("H2_PAL_NET_TLS_CASE ", file);
  case_json(file, item);
  fputc('\n', file);
  fflush(file);
}
void h2_net_tls_summary(FILE *file, const h2_net_tls_result_t *result, int rc,
                        int cleanup) {
  fprintf(
      file,
      "H2_PAL_NET_TLS_SUMMARY "
      "{\"profile\":\"net-tls-core\",\"operations\":21,\"core_qualified\":%s,"
      "\"full_net_qualified\":false,\"passed\":%u,\"mandatory_passed\":%u,"
      "\"failed\":%u,\"blocked\":%u,\"unsupported\":%u,\"not_assessed\":%u,"
      "\"retained_sockets\":%zu,\"retained_resolvers\":%zu,\"retained_"
      "allocations\":%zu,\"rc\":%d,\"teardown\":%d}\n",
      rc == H2_PAL_OK && cleanup == H2_PAL_OK && result->mandatory_passed == 36u
          ? "true"
          : "false",
      result->passed, result->mandatory_passed, result->failed, result->blocked,
      result->unsupported, result->not_assessed, result->retained_sockets,
      result->retained_resolvers, result->retained_allocations, rc, cleanup);
  fflush(file);
}
int h2_net_tls_write_report(const char *path, const char *platform,
                            const h2_net_tls_result_t *result, int rc,
                            int cleanup) {
  FILE *file = fopen(path, "w");
  if (!file)
    return H2_PAL_ERR_IO;
  fprintf(
      file,
      "{\"platform\":\"%s\",\"profile\":\"net-tls-core\",\"operations\":21,"
      "\"core_qualified\":%s,\"full_net_qualified\":false,\"passed\":%u,"
      "\"mandatory_passed\":%u,\"failed\":%u,\"blocked\":%u,\"unsupported\":%u,"
      "\"not_assessed\":%u,\"retained_sockets\":%zu,\"retained_resolvers\":%zu,"
      "\"retained_allocations\":%zu,\"rc\":%d,\"teardown\":%d,\"cases\":[",
      platform,
      rc == H2_PAL_OK && cleanup == H2_PAL_OK && result->mandatory_passed == 36u
          ? "true"
          : "false",
      result->passed, result->mandatory_passed, result->failed, result->blocked,
      result->unsupported, result->not_assessed, result->retained_sockets,
      result->retained_resolvers, result->retained_allocations, rc, cleanup);
  for (unsigned i = 0u; i < H2_NET_TLS_CASE_COUNT; ++i) {
    if (i)
      fputc(',', file);
    case_json(file, &result->cases[i]);
  }
  fputs("]}\n", file);
  int error = ferror(file);
  if (fclose(file) || error)
    return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}
