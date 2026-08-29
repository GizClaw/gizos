#ifndef H2_BLEIKCP_TASK_NAMES_H
#define H2_BLEIKCP_TASK_NAMES_H

#define H2_BLEIKCP_WORKER_TASK_NAME_VALUE "$bleikcp/kcp"
#define H2_BLEIKCP_SERVER_TASK_NAME_VALUE "$bleikcp/server"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_bleikcp_worker_task_name[sizeof(H2_BLEIKCP_WORKER_TASK_NAME_VALUE)];
extern const char
    h2_bleikcp_server_task_name[sizeof(H2_BLEIKCP_SERVER_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
