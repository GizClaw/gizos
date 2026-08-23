#ifndef H2_H2LOADER_WEB_H
#define H2_H2LOADER_WEB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Browser/WASM H2Loader client. */
typedef struct h2_h2loader_web_client h2_h2loader_web_client_t;

/** Create one client backed by the reusable Web PAL and Host Core. */
h2_h2loader_web_client_t *h2_h2loader_web_create(void);

/** Start the browser serial chooser from the current user-activation stack. */
int h2_h2loader_web_request_port(h2_h2loader_web_client_t *client);

/** Poll chooser completion and expose the selected process-local port id. */
int h2_h2loader_web_authorization_result(
    h2_h2loader_web_client_t *client);
const char *h2_h2loader_web_authorization_port(
    h2_h2loader_web_client_t *client);

/** Revoke one authorized port (SerialPort.forget) and poll its result. */
int h2_h2loader_web_forget_port(h2_h2loader_web_client_t *client,
                                const char *port_id);
int h2_h2loader_web_forget_result(h2_h2loader_web_client_t *client);

/** Start an asynchronous operation and return a generation-safe handle. */
uint32_t h2_h2loader_web_list_ports(
    h2_h2loader_web_client_t *client);
uint32_t h2_h2loader_web_inspect_package(
    h2_h2loader_web_client_t *client, uint32_t blob_handle,
    uint32_t blob_size);
uint32_t h2_h2loader_web_status(
    h2_h2loader_web_client_t *client, const char *port_id);
uint32_t h2_h2loader_web_install(
    h2_h2loader_web_client_t *client, const char *port_id,
    uint32_t blob_handle, uint32_t blob_size);
uint32_t h2_h2loader_web_stage(
    h2_h2loader_web_client_t *client, const char *port_id,
    uint32_t blob_handle, uint32_t blob_size);
uint32_t h2_h2loader_web_rollback(
    h2_h2loader_web_client_t *client, const char *port_id);
uint32_t h2_h2loader_web_restart(
    h2_h2loader_web_client_t *client, const char *port_id);
uint32_t h2_h2loader_web_reboot_loader(
    h2_h2loader_web_client_t *client, const char *port_id);
uint32_t h2_h2loader_web_reboot_app(
    h2_h2loader_web_client_t *client, const char *port_id);

/** Observe, cancel and release one asynchronous operation. */
int h2_h2loader_web_job_done(
    h2_h2loader_web_client_t *client, uint32_t handle);
int h2_h2loader_web_job_result(
    h2_h2loader_web_client_t *client, uint32_t handle);
const char *h2_h2loader_web_job_json(
    h2_h2loader_web_client_t *client, uint32_t handle);
const char *h2_h2loader_web_job_progress_json(
    h2_h2loader_web_client_t *client, uint32_t handle);
int h2_h2loader_web_job_cancel(
    h2_h2loader_web_client_t *client, uint32_t handle);
int h2_h2loader_web_job_release(
    h2_h2loader_web_client_t *client, uint32_t handle);

/** Begin bounded shutdown and advance it until it no longer WOULD_BLOCK. */
int h2_h2loader_web_close_begin(h2_h2loader_web_client_t *client);
int h2_h2loader_web_close_step(h2_h2loader_web_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
