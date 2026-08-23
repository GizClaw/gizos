#ifndef SCTP_H_
#define SCTP_H_

#include "dtls_srtp.h"
#include "h2/pal/net/h2_pal_sctp.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

typedef enum DecpMsgType {
  DATA_CHANNEL_OPEN = 0x03,
  DATA_CHANNEL_ACK = 0x02,
} DecpMsgType;

typedef enum DataChannelPpid {
  DATA_CHANNEL_PPID_CONTROL = 50,
  DATA_CHANNEL_PPID_DOMSTRING = 51,
  DATA_CHANNEL_PPID_BINARY_PARTIAL = 52,
  DATA_CHANNEL_PPID_BINARY = 53,
  DATA_CHANNEL_PPID_DOMSTRING_PARTIAL = 54,
} DataChannelPpid;

typedef enum SctpDataPpid {
  PPID_CONTROL = 50,
  PPID_STRING = 51,
  PPID_BINARY = 53,
  PPID_STRING_EMPTY = 56,
  PPID_BINARY_EMPTY = 57,
} SctpDataPpid;

typedef struct {
  char label[129];
  uint16_t sid;
  bool unordered;
  bool negotiated;
  bool remote_open_pending;
  h2_pal_sctp_reliability_t reliability;
  uint32_t reliability_value;
} SctpStreamEntry;

typedef struct SctpRemoteChannel {
  const char* label;
  size_t label_len;
  uint16_t sid;
  bool unordered;
  h2_pal_sctp_reliability_t reliability;
  uint32_t reliability_value;
} SctpRemoteChannel;

typedef struct Sctp {
  const h2_pal_log_api_t *log;
  const h2_pal_mem_api_t *mem;
  const h2_pal_sctp_api_t *api;
  const h2_pal_time_api_t *time;
  h2_pal_sctp_association_t* association;
  DtlsSrtp* dtls_srtp;
  int connected;
  int open_pending;
  size_t stream_count;
  size_t stream_capacity;
  SctpStreamEntry* stream_table;
  uint16_t remote_stream_first;
  int association_call_active;

  h2_pal_result_t (*onmessage)(char* msg, size_t len, void* userdata,
                               uint16_t sid, int is_text);
  void (*onopen)(void* userdata);
  void (*onclose)(void* userdata);
  void (*onstreamreset)(const h2_pal_sctp_stream_reset_event_t* event,
                        void* userdata);
  void (*onlocalchannelopen)(uint16_t sid, void* userdata);
  int (*onremotechannel)(const SctpRemoteChannel* channel, void* userdata);
  void* userdata;
} Sctp;

int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls_srtp);
void sctp_destroy_association(Sctp* sctp);
int sctp_service(Sctp* sctp);
int sctp_is_connected(Sctp* sctp);
h2_pal_result_t sctp_is_writable(Sctp* sctp, bool* out_writable);
void sctp_incoming_data(Sctp* sctp, char* buf, size_t len);
void sctp_parse_data_channel_open(Sctp* sctp, uint16_t sid, char* data,
                                  size_t length);
int sctp_register_data_channel(Sctp* sctp, const char* label, uint16_t sid,
                               uint8_t channel_type,
                               uint32_t reliability_parameter);
int sctp_unregister_data_channel(Sctp* sctp, uint16_t sid);
int sctp_outgoing_data(Sctp* sctp, char* buf, size_t len, SctpDataPpid ppid,
                       uint16_t sid);
int sctp_handle_incoming_data(Sctp* sctp, char* data, size_t len, uint32_t ppid,
                              uint16_t sid, int flags);
h2_pal_result_t sctp_close_stream(Sctp* sctp, uint16_t sid);
void sctp_onmessage(Sctp* sctp,
                    h2_pal_result_t (*onmessage)(char* msg, size_t len,
                                                 void* userdata, uint16_t sid,
                                                 int is_text));
void sctp_onopen(Sctp* sctp, void (*onopen)(void* userdata));
void sctp_onclose(Sctp* sctp, void (*onclose)(void* userdata));
void sctp_onstreamreset(
    Sctp* sctp,
    void (*onstreamreset)(const h2_pal_sctp_stream_reset_event_t* event,
                          void* userdata));
void sctp_onlocalchannelopen(
    Sctp* sctp, void (*onlocalchannelopen)(uint16_t sid, void* userdata));
void sctp_onremotechannel(
    Sctp* sctp,
    int (*onremotechannel)(const SctpRemoteChannel* channel, void* userdata));

#endif  // SCTP_H_
