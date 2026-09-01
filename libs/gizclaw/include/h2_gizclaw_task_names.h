#ifndef H2_GIZCLAW_TASK_NAMES_H
#define H2_GIZCLAW_TASK_NAMES_H

#define H2_GIZCLAW_NET_TASK_NAME_VALUE "$gizclaw/net"
#define H2_GIZCLAW_AUDIO_UPLINK_TASK_NAME_VALUE "$gizclaw/audio/uplink"
#define H2_GIZCLAW_AUDIO_DOWNLINK_TASK_NAME_VALUE "$gizclaw/audio/downlink"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_gizclaw_net_task_name[sizeof(H2_GIZCLAW_NET_TASK_NAME_VALUE)];
extern const char h2_gizclaw_audio_uplink_task_name[sizeof(
    H2_GIZCLAW_AUDIO_UPLINK_TASK_NAME_VALUE)];
extern const char h2_gizclaw_audio_downlink_task_name[sizeof(
    H2_GIZCLAW_AUDIO_DOWNLINK_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
