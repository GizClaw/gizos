#include "fixture.h"
#include "h2_pal_ipv6_fixture_config.h"
#include "h2_pal_ipv6_server_config.h"
#include "h2_sctp.h"
#include "peer.h"
#include "peer_connection.h"
#include "sctp.h"
#include <stdio.h>
#include <string.h>

const char h2_ipv6_fixture_task_name[] = "pal-ipv6/fixture/control";
const char h2_ipv6_fixture_worker_task_name[] = "pal-ipv6/fixture/raw";
const char h2_ipv6_fixture_http_task_name[] = "pal-ipv6/fixture/http";
const char h2_ipv6_fixture_mqtt_task_name[] = "pal-ipv6/fixture/mqtt";
const char h2_ipv6_fixture_udp_task_name[] = "pal-ipv6/fixture/udp";
const char *h2_ipv6_fixture_session(void) { return H2_PAL_IPV6_SESSION; }
const char *h2_ipv6_fixture_certificate(int expired) {
  return expired ? H2_PAL_IPV6_EXPIRED_HEX : H2_PAL_IPV6_LEAF_HEX;
}
const char *h2_ipv6_fixture_key(void) { return H2_PAL_IPV6_KEY_HEX; }
static h2_runtime_t *rt;
static const h2_ipv6_tls_server_api_t *tls_api;
static const h2_pal_dtls_api_t *dtls_api;
static h2_pal_net_addr_t host;
static h2_pal_mutex_t *record_lock;
typedef struct record {
  char id[51], run[17];
  unsigned mode;
  int listener, client;
  h2_pal_net_addr_t peer;
  h2_pal_task_t *worker;
  int accepted, valid, finished, closed;
  size_t received, sent;
  h2_ipv6_tls_evidence_t tls;
} record_t;
static record_t record;

static uint64_t now(void) {
  uint64_t value = 0;
  h2_pal_time_get_monotonic_ms(rt->time, &value);
  return value;
}
static void log_line(const char *message) {
  h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "ipv6-fixture", message);
}
static int listen_on(uint16_t port, int *fd) {
  h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR,
                            .source_addr = host};
  h2_pal_net_addr_t actual;
  int rc = h2_pal_net_tcp_listen(rt->net, H2_PAL_NET_FAMILY_IPV6, port, &bind,
                                 fd, &actual);
  return rc ? rc : (int)actual.port;
}
static int udp_on(uint16_t port, int *fd) {
  h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR,
                            .source_addr = host};
  h2_pal_net_addr_t actual;
  int rc = h2_pal_net_udp_open_bound(rt->net, H2_PAL_NET_FAMILY_IPV6, port,
                                     &bind, fd, &actual);
  return rc ? rc : (int)actual.port;
}
static int receive(int fd, void *data, size_t size) {
  return h2_pal_net_tcp_recv(rt->net, fd, data, size, 1000);
}
static int write_all(int fd, const void *data, size_t size) {
  const uint8_t *bytes = data;
  size_t offset = 0;
  while (offset < size) {
    int sent = h2_pal_net_tcp_send_timeout(rt->net, fd, bytes + offset,
                                           size - offset, 5000);
    if (sent <= 0)
      return sent < 0 ? sent : H2_PAL_ERR_IO;
    offset += (size_t)sent;
  }
  return H2_PAL_OK;
}
static uint8_t payload(size_t i) { return (uint8_t)((i * 37 + 11) % 251); }
static int check_payload(const uint8_t *data, size_t size) {
  char header[96] = {0};
  int length = snprintf(header, sizeof(header), "H2NETTLS/1 %s %s",
                        H2_PAL_IPV6_SESSION, record.id);
  if (length <= 0 || length >= 96 || size < 96 || memcmp(data, header, 96))
    return 0;
  for (size_t i = 96; i < size; ++i)
    if (data[i] != payload(i - 96))
      return 0;
  return 1;
}
static int same_peer(const h2_pal_net_addr_t *a, const h2_pal_net_addr_t *b) {
  return a->family == H2_PAL_NET_FAMILY_IPV6 && b->family == a->family &&
         a->scope_id == b->scope_id && !memcmp(a->ip, b->ip, 16);
}
static void raw_worker(void *unused) {
  (void)unused;
  uint8_t *buffer = h2_pal_mem_alloc(rt->mem, 96 + 4097);
  int client = -1, rc = H2_PAL_ERR_NO_MEMORY;
  void *tls = NULL;
  size_t got = 0, sent = 0;
  int accepted = 0, valid = 0, closed = 0;
  h2_ipv6_tls_evidence_t evidence = {0};
  if (!buffer)
    goto done;
  if (record.mode == 1) {
    h2_pal_net_addr_t peer;
    rc = h2_pal_net_udp_recvfrom(rt->net, record.listener, &peer, buffer,
                                 96 + 513, 3000);
    accepted = rc > 0 && same_peer(&peer, &record.peer);
    got = rc > 0 ? (size_t)rc : 0;
    valid = accepted && got == 96 + 513 && check_payload(buffer, got);
    if (valid) {
      for (size_t i = 0; i < 513; ++i)
        buffer[i] = payload(i) ^ 0xa5;
      rc = h2_pal_net_udp_sendto(rt->net, record.listener, &peer, buffer, 513);
      sent = rc > 0 ? (size_t)rc : 0;
    }
    goto done;
  }
  if (record.mode == 6) {
    rc = h2_pal_net_tcp_open_bound(rt->net, H2_PAL_NET_FAMILY_IPV6, NULL,
                                   &client);
    if (!rc)
      rc = h2_pal_net_tcp_connect(rt->net, client, &record.peer, 5000);
    accepted = !rc;
  } else {
    h2_pal_net_addr_t peer;
    rc = h2_pal_net_tcp_accept(rt->net, record.listener, &client, &peer, 3000);
    accepted = !rc && same_peer(&peer, &record.peer);
  }
  h2_pal_mutex_lock(rt->sync, record_lock);
  record.accepted = accepted;
  record.client = client;
  h2_pal_mutex_unlock(rt->sync, record_lock);
  if (!accepted)
    goto done;
  if (record.mode == 5) {
    closed = 1;
    goto done;
  }
  if (record.mode == 4) {
    rc = receive(client, buffer, 64);
    evidence.client_hello = rc > 5 && buffer[0] == 22 && buffer[5] == 1;
    h2_pal_time_sleep_ms(rt->time, 1000);
    goto done;
  }
  if (record.mode == 2 || record.mode == 3) {
    tls = tls_api->open(client, record.mode == 3, &evidence);
    if (!tls)
      goto done;
  }
  while (got < 96 + 4097) {
    rc = tls ? tls_api->read(tls, buffer + got, 96 + 4097 - got)
             : receive(client, buffer + got, 96 + 4097 - got);
    if (rc <= 0)
      break;
    got += (size_t)rc;
  }
  valid = got == 96 + 4097 && check_payload(buffer, got);
  if (valid) {
    for (size_t i = 0; i < 4097; ++i)
      buffer[i] = payload(i) ^ 0xa5;
    while (sent < 4097) {
      size_t part = 4097 - sent < 73 ? 4097 - sent : 73;
      rc = tls ? tls_api->write(tls, buffer + sent, part)
               : h2_pal_net_tcp_send_timeout(rt->net, client, buffer + sent,
                                             part, 5000);
      if (rc <= 0)
        break;
      sent += (size_t)rc;
    }
  }
done:
  if (tls)
    tls_api->close(tls);
  h2_pal_net_close(rt->net, client);
  h2_pal_mem_free(rt->mem, buffer);
  h2_pal_mutex_lock(rt->sync, record_lock);
  record.client = -1;
  record.accepted = accepted;
  record.received = got;
  record.sent = sent;
  record.valid = valid;
  record.closed = closed;
  record.tls = evidence;
  record.finished = 1;
  h2_pal_mutex_unlock(rt->sync, record_lock);
  char line[256];
  snprintf(line, sizeof(line),
           "H2_IPV6_FIXTURE_RAW session=%s run=%s case=%s family=6 accepted=%d "
           "valid=%d rx=%zu tx=%zu hello=%d cert=%d handshake=%d",
           H2_PAL_IPV6_SESSION, record.run, record.id, accepted, valid, got,
           sent, evidence.client_hello, evidence.certificate_presented,
           evidence.handshake);
  log_line(line);
}
static int hex_id(const char *s, size_t size) {
  if (strlen(s) != size)
    return 0;
  for (size_t i = 0; i < size; ++i)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
      return 0;
  return 1;
}
static int proof(unsigned kind) {
  int valid;
  h2_pal_mutex_lock(rt->sync, record_lock);
  valid = record.accepted;
  size_t expected = record.mode == 1 ? 513 : 4097;
  if (kind == 0 || kind == 2) {
    valid &= record.valid && record.received == expected + 96 &&
             record.sent == expected;
    if (record.mode == 2 || record.mode == 3)
      valid &= record.tls.handshake;
    if (kind == 2)
      valid &= record.tls.sni_alpn;
  } else if (kind == 1) {
    valid &= record.finished && record.tls.client_hello &&
             record.tls.certificate_presented && !record.received &&
             !record.sent;
  } else if (kind == 3)
    valid &=
        record.mode == 4 && record.tls.client_hello && !record.tls.handshake;
  else if (kind == 4)
    valid &= record.closed && record.finished;
  else if (kind != 5)
    valid = 0;
  h2_pal_mutex_unlock(rt->sync, record_lock);
  return valid;
}
static void retire_worker(void) {
  if (record.worker) {
    h2_pal_task_join(rt->task, record.worker);
    record.worker = NULL;
  }
  h2_pal_net_close(rt->net, record.listener);
}

/* The answerer uses H2Peer's existing portable protocol engine. This fixture
 * owns one thread and the engine directly; it does not change the public PAL
 * offer-only contract or use a host as an ICE/media proxy. */
typedef struct queued {
  uint8_t *data;
  size_t size;
  uint16_t sid;
  int text, audio;
} queued_t;
static PeerConnection *pc;
static queued_t outgoing[64];
static unsigned head, count, session_number;
static char peer_session[40];
static int queue_message(const void *data, size_t size, uint16_t sid, int text,
                         int audio) {
  if (count == 64)
    return H2_PAL_ERR_WOULD_BLOCK;
  const char *prefix = audio  ? ""
                       : text ? "server-echo-text:"
                              : "server-echo-binary:";
  size_t prefix_len = strlen(prefix);
  uint8_t *copy = h2_pal_mem_alloc(rt->mem, prefix_len + size + 1);
  if (!copy)
    return H2_PAL_ERR_NO_MEMORY;
  memcpy(copy, prefix, prefix_len);
  if (size)
    memcpy(copy + prefix_len, data, size);
  outgoing[(head + count) % 64] =
      (queued_t){copy, prefix_len + size, sid, text, audio};
  ++count;
  return H2_PAL_OK;
}
static h2_pal_result_t on_message(char *data, size_t size, void *user,
                                  uint16_t sid, int text) {
  (void)user;
  return queue_message(data, size, sid, text, 0);
}
static void on_audio(uint8_t *data, size_t size, void *user) {
  (void)user;
  queue_message(data, size, 0, 0, 1);
}
static void on_peer_state(PeerConnectionState state, void *user) {
  (void)state;
  (void)user;
}
static int on_remote_channel(const SctpRemoteChannel *channel, void *user) {
  (void)user;
  return channel && channel->label_len > 4 && channel->label_len <= 32 &&
                 !memcmp(channel->label, "pal/", 4)
             ? 0
             : -1;
}
static void peer_closed(void) {
  if (pc) {
    peer_connection_close(pc);
    peer_connection_destroy(pc);
    pc = NULL;
  }
  while (count) {
    h2_pal_mem_free(rt->mem, outgoing[head].data);
    head = (head + 1) % 64;
    --count;
  }
}
static void pump_peer(void) {
  if (!pc)
    return;
  peer_connection_loop(pc, 0);
  while (count) {
    queued_t *item = &outgoing[head];
    int rc = item->audio
                 ? peer_connection_send_audio(pc, item->data, item->size)
                 : sctp_outgoing_data(peer_connection_get_sctp(pc),
                                      (char *)item->data, item->size,
                                      item->text ? PPID_STRING : PPID_BINARY,
                                      item->sid);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      break;
    h2_pal_mem_free(rt->mem, item->data);
    head = (head + 1) % 64;
    --count;
  }
}
static int only_ipv6_host(void *user, const char *prefix,
                          h2_pal_net_addr_t *out) {
  (void)user;
  (void)prefix;
  memset(out, 0, sizeof(*out));
  return H2_PAL_ERR_UNAVAILABLE;
}
static int only_ipv6_host_family(void *user, const char *prefix,
                                 h2_pal_net_family_t family,
                                 h2_pal_net_addr_t *out) {
  (void)user;
  (void)prefix;
  memset(out, 0, sizeof(*out));
  if (family != H2_PAL_NET_FAMILY_IPV6)
    return H2_PAL_ERR_UNAVAILABLE;
  *out = host;
  return H2_PAL_OK;
}
static h2_pal_net_vtable_t peer_net_vtable;
static h2_pal_net_api_t peer_net;
static h2_sctp_t *sctp_provider;
static void http_reply(int fd, unsigned status, const char *headers,
                       const void *body, size_t size) {
  char header[256];
  int length = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %u OK\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
      status, size, headers ? headers : "");
  if (length > 0 && (size_t)length < sizeof(header) &&
      !write_all(fd, header, (size_t)length) && size)
    write_all(fd, body, size);
}
static void http_request(int fd) {
  char *request = h2_pal_mem_alloc(rt->mem, 16000);
  if (!request)
    return;
  size_t used = 0, body_offset = 0, body_length = 0;
  for (;;) {
    int got = receive(fd, request + used, 15999 - used);
    if (got <= 0)
      goto done;
    used += (size_t)got;
    request[used] = 0;
    char *end = strstr(request, "\r\n\r\n");
    if (end) {
      body_offset = (size_t)(end + 4 - request);
      char *length = strstr(request, "Content-Length:");
      if (!length)
        length = strstr(request, "content-length:");
      if (length && length < end) {
        unsigned value = 0;
        if (sscanf(length + 15, "%u", &value) != 1 || value > 12000)
          goto done;
        body_length = value;
      }
      if (used >= body_offset + body_length)
        break;
    }
    if (used == 15999)
      goto done;
  }
  char method[8], path[160];
  if (sscanf(request, "%7s %159s", method, path) != 2)
    goto done;
  if (!strcmp(method, "POST") && !strcmp(path, "/offer")) {
    peer_closed();
    PeerConfiguration config = {.log = rt->log,
                                .mem = rt->mem,
                                .allocator = rt->mem,
                                .net = &peer_net,
                                .time = rt->time,
                                .crypto = rt->crypto,
                                .dtls = dtls_api,
                                .sctp = h2_sctp_api(sctp_provider),
                                .audio_codec = CODEC_OPUS,
                                .datachannel = DATA_CHANNEL_BINARY,
                                .onaudiotrack = on_audio};
    pc = peer_connection_create(&config);
    if (!pc) {
      log_line("H2_IPV6_FIXTURE_SIGNAL_FAIL stage=create_peer");
      http_reply(fd, 500, NULL, NULL, 0);
      goto done;
    }
    peer_connection_oniceconnectionstatechange(pc, on_peer_state);
    peer_connection_ondatachannel(pc, on_message, NULL, NULL);
    peer_connection_onremotechannel(pc, on_remote_channel);
    /* The private engine creates its DTLS identity with local SDP. Prepare
     * that before installing the authenticated remote fingerprint. */
    const char *answer = peer_connection_create_answer(pc);
    if (!answer) {
      log_line("H2_IPV6_FIXTURE_SIGNAL_FAIL stage=create_answer");
      http_reply(fd, 500, NULL, NULL, 0);
      goto done;
    }
    if (peer_connection_set_remote_description(pc, request + body_offset,
                                               SDP_TYPE_OFFER)) {
      char line[200];
      snprintf(line, sizeof(line),
               "H2_IPV6_FIXTURE_SIGNAL_FAIL stage=remote_sdp body_length=%zu "
               "received=%zu starts_sdp=%d",
               body_length, used - body_offset,
               !strncmp(request + body_offset, "v=0", 3));
      log_line(line);
      http_reply(fd, 500, NULL, NULL, 0);
      goto done;
    }
    unsigned sid = 0;
    char *negotiated = strstr(request, "X-H2-Negotiated-ID:");
    if (negotiated) {
      if (sscanf(negotiated + 19, "%u", &sid) != 1 || sid > 65534 ||
          peer_connection_register_negotiated_channel(
              pc, DATA_CHANNEL_RELIABLE, 0, "pal/id", (uint16_t)sid))
        goto done;
    }
    snprintf(peer_session, sizeof(peer_session), "board-%u", ++session_number);
    char headers[100];
    snprintf(headers, sizeof(headers), "X-H2-Session-ID: %s\r\n", peer_session);
    http_reply(fd, 200, headers, answer, strlen(answer));
  } else {
    char wanted[160];
    snprintf(wanted, sizeof(wanted), "/session/%s/ice-pair", peer_session);
    if (!strcmp(path, wanted)) {
      h2_pal_net_addr_t local, remote;
      if (pc && !peer_connection_selected_pair(pc, &local, &remote)) {
        char body[100];
        int length = snprintf(body, sizeof(body),
                              "{\"local_family\":%d,\"remote_family\":%d}",
                              local.family == H2_PAL_NET_FAMILY_IPV6 ? 6 : 4,
                              remote.family == H2_PAL_NET_FAMILY_IPV6 ? 6 : 4);
        http_reply(fd, 200, NULL, body, (size_t)length);
        log_line("H2_IPV6_FIXTURE_ICE selected_local_family=6 "
                 "selected_remote_family=6");
      } else
        http_reply(fd, 409, NULL, NULL, 0);
    } else {
      snprintf(wanted, sizeof(wanted), "/session/%s/close", peer_session);
      if (!strcmp(method, "POST") && !strcmp(path, wanted)) {
        peer_closed();
        http_reply(fd, 204, NULL, NULL, 0);
      } else if (!strcmp(method, "GET") && strstr(path, H2_PAL_IPV6_SESSION)) {
        http_reply(fd, 200, NULL, H2_PAL_IPV6_SESSION, 32);
        log_line("H2_IPV6_FIXTURE_HTTP family=6 payload_valid=1");
      } else
        http_reply(fd, 404, NULL, NULL, 0);
    }
  }
done:
  h2_pal_mem_free(rt->mem, request);
}
static void http_worker(void *user) {
  int listener = *(int *)user;
  for (;;) {
    pump_peer();
    int client = -1;
    h2_pal_net_addr_t peer;
    int rc = h2_pal_net_tcp_accept(rt->net, listener, &client, &peer, 2);
    if (!rc && peer.family == H2_PAL_NET_FAMILY_IPV6)
      http_request(client);
    h2_pal_net_close(rt->net, client);
    h2_pal_time_sleep_ms(rt->time, 1);
  }
}
static int mqtt_packet(int fd, uint8_t *body, size_t cap, unsigned *kind) {
  uint8_t first, byte;
  size_t size = 0, shift = 0, used = 0;
  if (receive(fd, &first, 1) != 1)
    return H2_PAL_ERR_IO;
  *kind = first >> 4;
  do {
    if (shift >= 28 || receive(fd, &byte, 1) != 1)
      return H2_PAL_ERR_FORMAT;
    size |= (size_t)(byte & 127) << shift;
    shift += 7;
  } while (byte & 128);
  if (size > cap)
    return H2_PAL_ERR_NO_SPACE;
  while (used < size) {
    int got = receive(fd, body + used, size - used);
    if (got <= 0)
      return H2_PAL_ERR_IO;
    used += (size_t)got;
  }
  return (int)size;
}
static void mqtt_worker(void *user) {
  int listener = *(int *)user;
  for (;;) {
    int client = -1;
    h2_pal_net_addr_t peer;
    uint8_t body[512];
    unsigned kind;
    int rc = h2_pal_net_tcp_accept(rt->net, listener, &client, &peer, 1000);
    if (!rc) {
      int size = mqtt_packet(client, body, sizeof(body) - 1, &kind);
      if (size > 32 && kind == 1 &&
          !memcmp(body + size - 32, H2_PAL_IPV6_SESSION,
                  sizeof(H2_PAL_IPV6_SESSION) - 1)) {
        static const uint8_t ack[] = {0x20, 2, 0, 0};
        write_all(client, ack, sizeof(ack));
        size = mqtt_packet(client, body, sizeof(body), &kind);
        if (size == 42 && kind == 3 && body[0] == 0 && body[1] == 8 &&
            !memcmp(body + 2, "pal/ipv6", 8) &&
            !memcmp(body + 10, H2_PAL_IPV6_SESSION,
                    sizeof(H2_PAL_IPV6_SESSION) - 1) &&
            mqtt_packet(client, body, sizeof(body), &kind) == 0 && kind == 14)
          log_line(
              "H2_IPV6_FIXTURE_MQTT family=6 publish_valid=1 disconnected=1");
      }
    }
    h2_pal_net_close(rt->net, client);
  }
}
static void udp_worker(void *user) {
  int *sockets = user;
  for (;;) {
    uint8_t packet[1024];
    h2_pal_net_addr_t peer;
    int size = h2_pal_net_udp_recvfrom(rt->net, sockets[0], &peer, packet,
                                       sizeof(packet), 10);
    if (size > 16) {
      char name[100] = {0};
      size_t offset = 12, used = 0;
      while (offset < (size_t)size && packet[offset]) {
        unsigned length = packet[offset++];
        if (length > 63 || offset + length >= (size_t)size ||
            used + length + 1 >= sizeof(name))
          break;
        if (used)
          name[used++] = '.';
        memcpy(name + used, packet + offset, length);
        used += length;
        offset += length;
      }
      ++offset;
      char expected[80];
      snprintf(expected, sizeof(expected), "%s.ipv6.test", H2_PAL_IPV6_SESSION);
      if (!strcmp(name, expected) && offset + 4 == (size_t)size &&
          !memcmp(packet + offset, "\0\x1c\0\1", 4)) {
        packet[2] = 0x85;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        static const uint8_t answer[] = {0xc0, 12, 0, 28, 0, 1,
                                         0,    0,  0, 1,  0, 16};
        memcpy(packet + size, answer, sizeof(answer));
        memcpy(packet + size + sizeof(answer), host.ip, 16);
        h2_pal_net_udp_sendto(rt->net, sockets[0], &peer, packet,
                              (size_t)size + sizeof(answer) + 16);
        log_line("H2_IPV6_FIXTURE_DNS family=6 query_type=AAAA nonce_valid=1");
      }
    }
    size = h2_pal_net_udp_recvfrom(rt->net, sockets[1], &peer, packet,
                                   sizeof(packet), 10);
    if (size >= 20 && packet[0] == 0 && packet[1] == 1 &&
        !memcmp(packet + 4, "\x21\x12\xa4\x42", 4)) {
      packet[0] = 1;
      packet[1] = 1;
      packet[2] = 0;
      packet[3] = 24;
      packet[20] = 0;
      packet[21] = 0x20;
      packet[22] = 0;
      packet[23] = 20;
      packet[24] = 0;
      packet[25] = 2;
      uint16_t port = peer.port ^ 0x2112;
      packet[26] = (uint8_t)(port >> 8);
      packet[27] = (uint8_t)port;
      for (unsigned i = 0; i < 16; ++i)
        packet[28 + i] = peer.ip[i] ^ packet[4 + i];
      h2_pal_net_udp_sendto(rt->net, sockets[1], &peer, packet, 44);
    }
  }
}
int h2_ipv6_board_fixture_run(h2_runtime_t *runtime,
                              const h2_pal_dtls_api_t *dtls,
                              const h2_ipv6_tls_server_api_t *tls,
                              int (*ready)(h2_runtime_t *)) {
  rt = runtime;
  dtls_api = dtls;
  tls_api = tls;
  if (!hex_id(H2_PAL_IPV6_SESSION, 32) || !tls || !dtls)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_net_resolve_addr(rt->net, H2_PAL_IPV6_HOST, &host);
  if (rc || host.family != H2_PAL_NET_FAMILY_IPV6)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_mutex_config_t mutex = {.name = "pal-ipv6/fixture/record"};
  rc = h2_pal_mutex_create(rt->sync, &mutex, &record_lock);
  if (rc)
    return rc;
  h2_sctp_config_t sctp = {.mem = rt->mem, .crypto = rt->crypto};
  rc = h2_sctp_create(&sctp, &sctp_provider);
  if (rc)
    return rc;
  rc = peer_init(rt->mem, rt->crypto);
  if (rc)
    return rc;
  peer_net_vtable = *rt->net->vtable;
  peer_net_vtable.get_host_addr = only_ipv6_host;
  peer_net_vtable.get_host_addr_family = only_ipv6_host_family;
  peer_net = (h2_pal_net_api_t){rt->net->user, &peer_net_vtable};
  static int control, http, mqtt, udp[2];
  rc = listen_on((uint16_t)H2_PAL_IPV6_PORT, &control);
  if (rc < 0)
    return rc;
  rc = listen_on(18080, &http);
  if (rc < 0)
    return rc;
  rc = listen_on((uint16_t)H2_PAL_IPV6_MQTT_PORT, &mqtt);
  if (rc < 0)
    return rc;
  rc = udp_on((uint16_t)H2_PAL_IPV6_DNS_PORT, &udp[0]);
  if (rc < 0)
    return rc;
  rc = udp_on(13478, &udp[1]);
  if (rc < 0)
    return rc;
  h2_pal_task_t *task;
  h2_pal_task_options_t options = {.name = h2_ipv6_fixture_http_task_name};
  rc = h2_pal_task_start(rt->task, &options, http_worker, &http, &task);
  if (!rc) {
    options.name = h2_ipv6_fixture_mqtt_task_name;
    rc = h2_pal_task_start(rt->task, &options, mqtt_worker, &mqtt, &task);
  }
  if (!rc) {
    options.name = h2_ipv6_fixture_udp_task_name;
    rc = h2_pal_task_start(rt->task, &options, udp_worker, udp, &task);
  }
  if (rc)
    return rc;
  log_line(
      "H2_IPV6_FIXTURE_READY board=amoled host=fd53:697a:6f73:626::1 family=6");
  if (ready && (rc = ready(rt)) != H2_PAL_OK)
    return rc;
  record.listener = record.client = -1;
  for (;;) {
    int client = -1;
    h2_pal_net_addr_t peer;
    rc = h2_pal_net_tcp_accept(rt->net, control, &client, &peer, 1000);
    if (rc)
      continue;
    char request[200] = {0};
    size_t used = 0;
    while (used < sizeof(request) - 1 && !strchr(request, '\n')) {
      int got = receive(client, request + used, sizeof(request) - 1 - used);
      if (got <= 0)
        break;
      used += (size_t)got;
    }
    char action[8], session[33], run[17], id[51];
    unsigned mode = 0, callback = 0;
    int fields = sscanf(request, "%7s %32s %16s %50s %u %u", action, session,
                        run, id, &mode, &callback);
    int port = -1;
    if (fields >= 5 && !strcmp(session, H2_PAL_IPV6_SESSION) &&
        hex_id(run, 16) && peer.family == H2_PAL_NET_FAMILY_IPV6) {
      if (fields == 6 && !strcmp(action, "ARM") && mode < 7 &&
          ((mode == 6) == (callback != 0)) && callback <= 65535) {
        retire_worker();
        memset(&record, 0, sizeof(record));
        memcpy(record.id, id, strlen(id) + 1);
        memcpy(record.run, run, 17);
        record.mode = mode;
        record.peer = peer;
        record.client = record.listener = -1;
        port = mode == 6   ? (int)callback
               : mode == 1 ? udp_on(0, &record.listener)
                           : listen_on(0, &record.listener);
        if (mode == 6)
          record.peer.port = (uint16_t)callback;
        if (port > 0) {
          options.name = h2_ipv6_fixture_worker_task_name;
          rc = h2_pal_task_start(rt->task, &options, raw_worker, NULL,
                                 &record.worker);
          if (rc)
            port = -1;
        }
      } else if (fields == 5 && !strcmp(action, "PROOF") &&
                 !strcmp(record.id, id) && !strcmp(record.run, run)) {
        uint64_t deadline = now() + 2000;
        while (!proof(mode) && now() < deadline)
          h2_pal_time_sleep_ms(rt->time, 10);
        if (proof(mode))
          port = 0;
      }
    }
    char reply[32];
    int length =
        snprintf(reply, sizeof(reply), "%s %u\n", port < 0 ? "FAIL" : "OK",
                 port < 0 ? 0 : (unsigned)port);
    write_all(client, reply, (size_t)length);
    h2_pal_net_close(rt->net, client);
  }
}
