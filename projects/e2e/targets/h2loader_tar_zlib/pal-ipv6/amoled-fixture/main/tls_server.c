#include "tls_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct tls_server {
  int fd;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_x509_crt certificate;
  mbedtls_pk_context key;
  h2_ipv6_tls_evidence_t *evidence;
} tls_server_t;
#if MBEDTLS_VERSION_MAJOR < 4
static int entropy(void *user, unsigned char *data, size_t size) {
  (void)user;
  esp_fill_random(data, size);
  return 0;
}
#endif
static int bio_send(void *user, const unsigned char *data, size_t size) {
  tls_server_t *server = user;
  int result = send(server->fd, data, size, 0);
  return result >= 0 ? result
         : errno == EAGAIN || errno == EWOULDBLOCK
             ? MBEDTLS_ERR_SSL_WANT_WRITE
             : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int bio_receive(void *user, unsigned char *data, size_t size) {
  tls_server_t *server = user;
  int result = recv(server->fd, data, size, 0);
  return result >= 0 ? result
         : errno == EAGAIN || errno == EWOULDBLOCK
             ? MBEDTLS_ERR_SSL_WANT_READ
             : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int sni(void *user, mbedtls_ssl_context *ssl, const unsigned char *name,
               size_t size) {
  tls_server_t *server = user;
  (void)ssl;
  server->evidence->client_hello = 1;
  server->evidence->sni_alpn =
      size == 16 && !memcmp(name, "pal-net-tls.test", 16);
  return 0;
}
static unsigned char *decode(const char *hex, size_t *size) {
  size_t length = strlen(hex);
  if (!length || length % 2 || length > 20000)
    return NULL;
  unsigned char *data = calloc(1, length / 2 + 1);
  if (!data)
    return NULL;
  for (size_t i = 0; i < length; i += 2) {
    char pair[3] = {hex[i], hex[i + 1], 0}, *end;
    unsigned long value = strtoul(pair, &end, 16);
    if (*end) {
      free(data);
      return NULL;
    }
    data[i / 2] = (unsigned char)value;
  }
  *size = length / 2 + 1;
  return data;
}
static void close_server(void *user) {
  tls_server_t *server = user;
  mbedtls_ssl_free(&server->ssl);
  mbedtls_ssl_config_free(&server->config);
  mbedtls_x509_crt_free(&server->certificate);
  mbedtls_pk_free(&server->key);
  free(server);
}
static void *open_server(int fd, int expired,
                         h2_ipv6_tls_evidence_t *evidence) {
  tls_server_t *server = calloc(1, sizeof(*server));
  if (!server)
    return NULL;
  server->fd = fd;
  server->evidence = evidence;
  mbedtls_ssl_init(&server->ssl);
  mbedtls_ssl_config_init(&server->config);
  mbedtls_x509_crt_init(&server->certificate);
  mbedtls_pk_init(&server->key);
  size_t certificate_size = 0, key_size = 0;
  unsigned char *certificate =
      decode(h2_ipv6_fixture_certificate(expired), &certificate_size);
  unsigned char *key = decode(h2_ipv6_fixture_key(), &key_size);
  int rc = !certificate || !key
               ? -1
               : mbedtls_x509_crt_parse(&server->certificate, certificate,
                                        certificate_size);
  if (!rc)
    rc = psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
  if (!rc) {
#if MBEDTLS_VERSION_MAJOR < 4
    rc = mbedtls_pk_parse_key(&server->key, key, key_size, NULL, 0, entropy,
                              NULL);
#else
    rc = mbedtls_pk_parse_key(&server->key, key, key_size, NULL, 0);
#endif
  }
  free(certificate);
  if (key) {
    memset(key, 0, key_size);
    free(key);
  }
  if (!rc)
    rc = mbedtls_ssl_config_defaults(&server->config, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
  if (!rc) {
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&server->config, entropy, NULL);
#endif
    mbedtls_ssl_conf_authmode(&server->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_tls_version(&server->config,
                                     MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&server->config,
                                     MBEDTLS_SSL_VERSION_TLS1_2);
    static const char *alpn[] = {"h2-pal-e2e", NULL};
    rc = mbedtls_ssl_conf_alpn_protocols(&server->config, alpn);
    mbedtls_ssl_conf_sni(&server->config, sni, server);
  }
  if (!rc)
    rc = mbedtls_ssl_conf_own_cert(&server->config, &server->certificate,
                                   &server->key);
  if (!rc)
    rc = mbedtls_ssl_setup(&server->ssl, &server->config);
  if (!rc) {
    mbedtls_ssl_set_bio(&server->ssl, server, bio_send, bio_receive, NULL);
    int64_t deadline = esp_timer_get_time() + 20000000;
    while (!mbedtls_ssl_is_handshake_over(&server->ssl) &&
           esp_timer_get_time() < deadline) {
      int state = server->ssl.MBEDTLS_PRIVATE(state);
      rc = mbedtls_ssl_handshake_step(&server->ssl);
      if (!rc && state == MBEDTLS_SSL_CLIENT_HELLO)
        evidence->client_hello = 1;
      if (!rc && state == MBEDTLS_SSL_SERVER_CERTIFICATE)
        evidence->certificate_presented = 1;
      if (rc && rc != MBEDTLS_ERR_SSL_WANT_READ &&
          rc != MBEDTLS_ERR_SSL_WANT_WRITE)
        break;
      rc = 0;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!rc && mbedtls_ssl_is_handshake_over(&server->ssl)) {
      evidence->handshake = 1;
      const char *protocol = mbedtls_ssl_get_alpn_protocol(&server->ssl);
      evidence->sni_alpn &= protocol && !strcmp(protocol, "h2-pal-e2e");
      return server;
    }
  }
  close_server(server);
  return NULL;
}
static int read_server(void *user, uint8_t *data, size_t size) {
  tls_server_t *server = user;
  int rc;
  int64_t deadline = esp_timer_get_time() + 1000000;
  do {
    rc = mbedtls_ssl_read(&server->ssl, data, size);
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
      return rc;
    vTaskDelay(pdMS_TO_TICKS(1));
  } while (esp_timer_get_time() < deadline);
  return H2_PAL_ERR_TIMEOUT;
}
static int write_server(void *user, const uint8_t *data, size_t size) {
  tls_server_t *server = user;
  int rc;
  int64_t deadline = esp_timer_get_time() + 5000000;
  do {
    rc = mbedtls_ssl_write(&server->ssl, data, size);
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
      return rc;
    vTaskDelay(pdMS_TO_TICKS(1));
  } while (esp_timer_get_time() < deadline);
  return H2_PAL_ERR_TIMEOUT;
}
const h2_ipv6_tls_server_api_t h2_ipv6_tls_server = {
    open_server, read_server, write_server, close_server};
