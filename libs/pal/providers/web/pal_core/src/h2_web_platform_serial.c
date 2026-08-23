#include "h2_web_platform_internal.h"

#include <errno.h>
#include <limits.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_SERIAL_OPERATION_TIMEOUT_MS 10000u

typedef struct h2_web_serial_operation {
  uint64_t token;
  h2_pal_result_t result;
  size_t count;
  bool pending;
  bool done;
  bool wake_recorded;
} h2_web_serial_operation_t;

struct h2_pal_serial_host_snapshot {
  size_t count;
  h2_pal_serial_host_port_info_t *ports;
};

struct h2_pal_serial_host_session {
  h2_web_platform_t *owner;
  h2_pal_serial_host_session_t *next;
  h2_pal_uart_io_stream_config_t config;
  h2_pal_uart_io_stream_api_t stream;
  h2_web_serial_operation_t operation;
  bool opened;
  bool closing;
};

typedef struct h2_web_serial_state {
  h2_web_platform_t *owner;
  h2_pal_serial_host_session_t *sessions;
  h2_pal_serial_host_port_info_t *scan_ports;
  size_t scan_count;
  uint64_t scan_token;
  h2_pal_result_t scan_result;
  bool scan_pending;
  bool scan_done;
  bool scan_wake_recorded;
  uint64_t authorization_token;
  h2_pal_result_t authorization_result;
  char authorized_port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  bool authorization_pending;
  bool authorization_terminal;
  uint64_t forget_token;
  h2_pal_result_t forget_result;
  bool forget_pending;
  bool forget_terminal;
  bool shutting_down;
  h2_pal_result_t shutdown_result;
} h2_web_serial_state_t;

static h2_web_serial_state_t *h2_web_serial_state(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : platform->serial_state;
}

static bool h2_web_serial_session_shutting_down(
    const h2_pal_serial_host_session_t *session) {
  h2_web_serial_state_t *state = session == NULL
      ? NULL : h2_web_serial_state(session->owner);
  return state == NULL || state->shutting_down;
}

static h2_pal_serial_host_session_t *h2_web_serial_find_session(
    h2_web_serial_state_t *state, uintptr_t address) {
  for (h2_pal_serial_host_session_t *session = state->sessions;
       session != NULL; session = session->next) {
    if ((uintptr_t)session == address) {
      return session;
    }
  }
  return NULL;
}

EM_JS(void, h2_web_serial_platform_register_js,
      (uintptr_t platform_address), {
        const platforms = Module['h2WebSerialPlatforms'] ||= new Map();
        platforms.set(platform_address, {});
      });

EM_JS(void, h2_web_serial_platform_unregister_js,
      (uintptr_t platform_address), {
        const platforms = Module['h2WebSerialPlatforms'];
        if (platforms) platforms.delete(platform_address);
      });

EM_JS(void, h2_web_serial_scan_js,
      (uintptr_t platform_address, uint64_t token), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_scan_complete'](platform_address, token,
                                                    result);
          }
        };
        if (!globalThis.isSecureContext || !globalThis.navigator ||
            !navigator.serial) {
          complete(-3);
          return;
        }
        const state = Module['h2WebSerialState'] ||= {
          nextPort: 1,
          ports: new Map(),
          sessions: new Map(),
        };
        const portNumber = (port) => {
          for (const [number, current] of state.ports) {
            if (current === port) return number;
          }
          const number = state.nextPort++;
          state.ports.set(number, port);
          return number;
        };
        navigator.serial.getPorts().then((ports) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (!platforms || platforms.get(platform_address) !== platform) return;
          const connected = ports.filter((port) => port.connected !== false);
          if (!Module['_h2_web_serial_scan_reset'](
                  platform_address, token, connected.length)) {
            complete(-5);
            return;
          }
          connected.forEach((port, index) => {
            const info = port.getInfo ? port.getInfo() : {};
            Module['_h2_web_serial_scan_port'](
                platform_address, token, index, portNumber(port),
                info.usbVendorId || 0, info.usbProductId || 0,
                info.usbVendorId === undefined ? 0 : 1,
                info.usbProductId === undefined ? 0 : 1,
                typeof port.setSignals === "function" ? 1 : 0);
          });
          complete(0);
        }).catch(() => complete(-4));
      });

EM_JS(void, h2_web_serial_request_port_js,
      (uintptr_t platform_address, uint64_t token), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result, number) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_authorization_complete'](
                platform_address, token, result, number || 0);
          }
        };
        if (!globalThis.isSecureContext || !globalThis.navigator ||
            !navigator.serial) {
          complete(-3, 0);
          return;
        }
        const state = Module['h2WebSerialState'] ||= {
          nextPort: 1,
          ports: new Map(),
          sessions: new Map(),
        };
        const portNumber = (port) => {
          for (const [number, current] of state.ports) {
            if (current === port) return number;
          }
          const number = state.nextPort++;
          state.ports.set(number, port);
          return number;
        };
        navigator.serial.requestPort().then(
          (port) => complete(0, portNumber(port)),
          (error) => complete(error && error.name === "NotFoundError" ? -8 : -2,
                              0));
      });

EM_JS(void, h2_web_serial_forget_port_js,
      (uintptr_t platform_address, uint64_t token, unsigned int port_number), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_forget_complete'](platform_address, token,
                                                      result);
          }
        };
        const state = Module['h2WebSerialState'];
        const port = state ? state.ports.get(port_number) : undefined;
        if (!port) {
          complete(-8);
          return;
        }
        if (typeof port.forget !== "function") {
          complete(-3);
          return;
        }
        Promise.resolve().then(() => port.forget()).then(() => {
          if (state.ports.get(port_number) === port) {
            state.ports.delete(port_number);
          }
          complete(0);
        }, () => complete(-4));
      });

EM_JS(void, h2_web_serial_open_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token,
       unsigned int port_number, uint32_t baud_rate, int data_bits,
       int stop_bits, int parity, int flow_control, size_t rx_buffer_size), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const port = state && state.ports.get(port_number);
        if (!port || port.connected === false) {
          complete(-8);
          return;
        }
        const options = {
          baudRate: baud_rate,
          dataBits: data_bits,
          stopBits: stop_bits,
          parity: parity === 1 ? "even" : (parity === 2 ? "odd" : "none"),
          flowControl: flow_control ? "hardware" : "none",
          bufferSize: Number(rx_buffer_size),
        };
        port.open(options).then(() => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (!platforms || platforms.get(platform_address) !== platform ||
              !Module['_h2_web_serial_session_token_valid'](
                  platform_address, session_address, token)) {
            port.close().catch(() => {});
            return;
          }
          state.sessions.set(session_address, {
            port,
            pending: new Uint8Array(0),
            pendingRead: null,
            rxLimit: Number(rx_buffer_size),
            terminal: false,
            reader: null,
            writer: null,
          });
          complete(0);
        }).catch((error) => complete(
            error && (error.name === "NetworkError" ||
                      error.name === "InvalidStateError" ||
                      error.name === "NotAllowedError" ||
                      error.name === "SecurityError") ? -2 : -4));
      });

EM_JS(void, h2_web_serial_read_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token,
       uintptr_t buffer, size_t length, uint32_t timeout_ms), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result, count) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, count || 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const session = state && state.sessions.get(session_address);
        if (!session || session.terminal || !session.port.readable) {
          complete(-10, 0);
          return;
        }
        const copy = (bytes) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (!platforms || platforms.get(platform_address) !== platform ||
              !Module['_h2_web_serial_session_token_valid'](
                  platform_address, session_address, token)) return;
          const count = Math.min(Number(length), bytes.byteLength);
          HEAPU8.set(bytes.subarray(0, count), buffer);
          session.pending = bytes.slice(count);
          complete(0, count);
        };
        if (session.pending.byteLength) {
          copy(session.pending);
          return;
        }
        (async () => {
          let timer;
          try {
            if (!session.pendingRead) {
              const reader = session.port.readable.getReader();
              session.reader = reader;
              session.pendingRead = reader.read().then(
                (value) => ({value}),
                (error) => ({error})).finally(() => {
                  try { reader.releaseLock(); } catch (_) {}
                  if (session.reader === reader) session.reader = null;
                });
            }
            const timeout = new Promise((resolve) => {
              timer = setTimeout(() => resolve({timeout: true}), timeout_ms);
            });
            const result = await Promise.race([session.pendingRead, timeout]);
            if (result.timeout) {
              complete(-6, 0);
              return;
            }
            session.pendingRead = null;
            if (result.error) {
              const closed = session.port.connected === false ||
                             result.error.name === "NetworkError";
              if (closed) session.terminal = true;
              complete(closed ? -10 : -4, 0);
            } else if (result.value.done) {
              complete(-10, 0);
            } else if (result.value.value.byteLength >
                       session.rxLimit + Number(length)) {
              session.terminal = true;
              complete(-13, 0);
            } else {
              copy(result.value.value);
            }
          } catch (error) {
            const closed = session.port.connected === false ||
                           (error && error.name === "NetworkError");
            if (closed) session.terminal = true;
            complete(closed ? -10 : -4, 0);
          } finally {
            clearTimeout(timer);
          }
        })();
      });

EM_JS(void, h2_web_serial_write_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token,
       uintptr_t buffer, size_t length, uint32_t timeout_ms), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result, count) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, count || 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const session = state && state.sessions.get(session_address);
        if (!session || session.terminal || !session.port.writable) {
          complete(-10, 0);
          return;
        }
        const bytes = HEAPU8.slice(buffer, buffer + Number(length));
        (async () => {
          let writer;
          let timer;
          try {
            writer = session.port.writable.getWriter();
            session.writer = writer;
            const timeout = new Promise((resolve) => {
              timer = setTimeout(() => resolve(false), timeout_ms);
            });
            const wrote = writer.write(bytes).then(() => true);
            const completed = await Promise.race([wrote, timeout]);
            if (!completed) {
              session.terminal = true;
              writer.abort().catch(() => {});
            }
            complete(completed ? 0 : -6, completed ? Number(length) : 0);
          } catch (error) {
            const closed = session.port.connected === false ||
                           (error && error.name === "NetworkError");
            if (closed) session.terminal = true;
            complete(closed ? -10 : -4, 0);
          } finally {
            clearTimeout(timer);
            if (writer) {
              try { writer.releaseLock(); } catch (_) {}
              if (session.writer === writer) session.writer = null;
            }
          }
        })();
      });

EM_JS(void, h2_web_serial_flush_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const session = state && state.sessions.get(session_address);
        if (!session || session.terminal || !session.port.writable) {
          complete(-10);
          return;
        }
        (async () => {
          let writer;
          let timer;
          try {
            writer = session.port.writable.getWriter();
            session.writer = writer;
            const timeout = new Promise((resolve) => {
              timer = setTimeout(() => resolve(false), 5000);
            });
            const ready = writer.ready.then(() => true);
            complete(await Promise.race([ready, timeout]) ? 0 : -6);
          } catch (_) {
            complete(-4);
          } finally {
            clearTimeout(timer);
            if (writer) {
              try { writer.releaseLock(); } catch (_) {}
              if (session.writer === writer) session.writer = null;
            }
          }
        })();
      });

EM_JS(void, h2_web_serial_signals_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token,
       int set_dtr, int dtr, int set_rts, int rts), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const session = state && state.sessions.get(session_address);
        if (!session || session.terminal ||
            typeof session.port.setSignals !== "function") {
          complete(-3);
          return;
        }
        const signals = {};
        if (set_dtr) signals.dataTerminalReady = !!dtr;
        if (set_rts) signals.requestToSend = !!rts;
        session.port.setSignals(signals).then(
          () => complete(0), () => complete(-4));
      });

EM_JS(void, h2_web_serial_close_js,
      (uintptr_t platform_address, uintptr_t session_address, uint64_t token), {
        const platform = Module['h2WebSerialPlatforms']?.get(platform_address);
        const complete = (result) => {
          const platforms = Module['h2WebSerialPlatforms'];
          if (platforms && platforms.get(platform_address) === platform) {
            Module['_h2_web_serial_session_complete'](
                platform_address, session_address, token, result, 0);
          }
        };
        const state = Module['h2WebSerialState'];
        const session = state && state.sessions.get(session_address);
        if (!session) {
          complete(0);
          return;
        }
        (async () => {
          try {
            if (session.reader) await session.reader.cancel();
            await session.port.close();
            state.sessions.delete(session_address);
            complete(0);
          } catch (_) {
            complete(-4);
          }
        })();
      });

EM_JS(void, h2_web_serial_force_close_js, (uintptr_t session_address), {
  const state = Module['h2WebSerialState'];
  const session = state && state.sessions.get(session_address);
  if (session) {
    state.sessions.delete(session_address);
    session.terminal = true;
    const settle = (operation) => {
      try {
        return Promise.resolve(operation()).catch(() => {});
      } catch (_) {
        return Promise.resolve();
      }
    };
    const close = () => settle(() => session.port.close());
    if (session.reader) {
      const reader = session.reader;
      session.reader = null;
      settle(() => reader.cancel());
      try { reader.releaseLock(); } catch (_) {}
    }
    if (session.writer) {
      const writer = session.writer;
      session.writer = null;
      settle(() => writer.abort());
      try { writer.releaseLock(); } catch (_) {}
    }
    close();
  }
});

EMSCRIPTEN_KEEPALIVE int h2_web_serial_scan_reset(uintptr_t platform_address,
                                                  uint64_t token,
                                                  size_t count) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  if (state == NULL || !state->scan_pending || token != state->scan_token ||
      count > SIZE_MAX / sizeof(*state->scan_ports)) {
    return 0;
  }
  free(state->scan_ports);
  state->scan_ports = count == 0u ? NULL : calloc(count, sizeof(*state->scan_ports));
  state->scan_count = state->scan_ports == NULL && count != 0u ? 0u : count;
  return count == 0u || state->scan_ports != NULL;
}

EMSCRIPTEN_KEEPALIVE void h2_web_serial_scan_port(
    uintptr_t platform_address, uint64_t token, size_t index,
    unsigned int port_number, unsigned int vid, unsigned int pid, int has_vid,
    int has_pid, int has_signals) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  if (state == NULL || !state->scan_pending || token != state->scan_token ||
      index >= state->scan_count) {
    return;
  }
  h2_pal_serial_host_port_info_t *info = &state->scan_ports[index];
  (void)snprintf(info->port_id, sizeof(info->port_id), "web-serial-%u",
                 port_number);
  (void)snprintf(info->endpoint, sizeof(info->endpoint), "web-serial-%u",
                 port_number);
  if (has_vid) {
    info->usb_vid = (uint16_t)vid;
    info->valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID;
  }
  if (has_pid) {
    info->usb_pid = (uint16_t)pid;
    info->valid_fields |= H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID;
  }
  if (has_signals) {
    info->capabilities =
        H2_PAL_SERIAL_HOST_CAP_DTR | H2_PAL_SERIAL_HOST_CAP_RTS;
  }
}

EMSCRIPTEN_KEEPALIVE void h2_web_serial_scan_complete(
    uintptr_t platform_address, uint64_t token, int result) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  if (state == NULL || !state->scan_pending || token != state->scan_token) {
    return;
  }
  state->scan_result = (h2_pal_result_t)result;
  state->scan_done = true;
  h2_web_platform_request_pump((h2_web_platform_t *)platform_address, 0u);
}

EMSCRIPTEN_KEEPALIVE void h2_web_serial_authorization_complete(
    uintptr_t platform_address, uint64_t token, int result,
    unsigned int port_number) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  if (state == NULL || !state->authorization_pending ||
      token != state->authorization_token) {
    return;
  }
  state->authorization_pending = false;
  state->authorization_terminal = true;
  state->authorization_result = (h2_pal_result_t)result;
  state->authorized_port_id[0] = '\0';
  if (result == H2_PAL_OK) {
    (void)snprintf(state->authorized_port_id,
                   sizeof(state->authorized_port_id), "web-serial-%u",
                   port_number);
  }
  h2_web_platform_request_pump((h2_web_platform_t *)platform_address, 0u);
}

EMSCRIPTEN_KEEPALIVE void h2_web_serial_forget_complete(
    uintptr_t platform_address, uint64_t token, int result) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  if (state == NULL || !state->forget_pending || token != state->forget_token) {
    return;
  }
  state->forget_pending = false;
  state->forget_terminal = true;
  state->forget_result = (h2_pal_result_t)result;
  h2_web_platform_request_pump((h2_web_platform_t *)platform_address, 0u);
}

EMSCRIPTEN_KEEPALIVE void h2_web_serial_session_complete(
    uintptr_t platform_address, uintptr_t session_address, uint64_t token,
    int result, size_t count) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  h2_pal_serial_host_session_t *session = state == NULL
      ? NULL : h2_web_serial_find_session(state, session_address);
  if (session == NULL || !session->operation.pending ||
      token != session->operation.token) {
    return;
  }
  session->operation.result = (h2_pal_result_t)result;
  session->operation.count = count;
  session->operation.done = true;
  h2_web_platform_request_pump((h2_web_platform_t *)platform_address, 0u);
}

EMSCRIPTEN_KEEPALIVE int h2_web_serial_session_token_valid(
    uintptr_t platform_address, uintptr_t session_address, uint64_t token) {
  h2_web_serial_state_t *state = h2_web_serial_state(
      (h2_web_platform_t *)platform_address);
  h2_pal_serial_host_session_t *session = state == NULL
      ? NULL : h2_web_serial_find_session(state, session_address);
  return session != NULL && session->operation.pending &&
         token == session->operation.token;
}

h2_libco_result_t h2_web_platform_serial_poll(h2_web_platform_t *platform,
                                               h2_libco_t *executor) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL) {
    return H2_LIBCO_OK;
  }
  if (state->scan_pending && state->scan_done &&
      !state->scan_wake_recorded) {
    state->scan_wake_recorded = true;
    (void)h2_libco_wake(executor, (uintptr_t)&state->scan_pending,
                        H2_LIBCO_WAKE_ALL, NULL);
  }
  for (h2_pal_serial_host_session_t *session = state->sessions;
       session != NULL; session = session->next) {
    if (session->operation.pending && session->operation.done &&
        !session->operation.wake_recorded) {
      session->operation.wake_recorded = true;
      (void)h2_libco_wake(executor, (uintptr_t)&session->operation,
                          H2_LIBCO_WAKE_ALL, NULL);
    }
  }
  return H2_LIBCO_OK;
}

static h2_pal_result_t h2_web_serial_wait(
    h2_pal_serial_host_session_t *session, uint32_t timeout_ms) {
  const h2_libco_result_t wait_result = h2_libco_wait(
      session->owner->executor, (uintptr_t)&session->operation, timeout_ms);
  if (wait_result != H2_LIBCO_WOKEN || !session->operation.done) {
    ++session->operation.token;
    session->operation.pending = false;
    (void)h2_libco_wake(session->owner->executor,
                        (uintptr_t)&session->closing,
                        H2_LIBCO_WAKE_ALL, NULL);
    return wait_result == H2_LIBCO_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                               : H2_PAL_ERR_INVALID_STATE;
  }
  session->operation.pending = false;
  (void)h2_libco_wake(session->owner->executor,
                      (uintptr_t)&session->closing,
                      H2_LIBCO_WAKE_ALL, NULL);
  return session->operation.result;
}

static uint64_t h2_web_serial_begin(h2_pal_serial_host_session_t *session) {
  ++session->operation.token;
  session->operation = (h2_web_serial_operation_t){
      .token = session->operation.token,
      .result = H2_PAL_ERR_WOULD_BLOCK,
      .pending = true,
  };
  return session->operation.token;
}

static h2_pal_result_t h2_web_serial_scan(
    void *user, h2_pal_serial_host_snapshot_t **out_snapshot) {
  h2_web_platform_t *platform = user;
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (out_snapshot != NULL) {
    *out_snapshot = NULL;
  }
  if (state == NULL || out_snapshot == NULL || state->scan_pending) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state->shutting_down) return H2_PAL_ERR_CLOSED;
  free(state->scan_ports);
  state->scan_ports = NULL;
  state->scan_count = 0u;
  ++state->scan_token;
  state->scan_pending = true;
  state->scan_done = false;
  state->scan_wake_recorded = false;
  state->scan_result = H2_PAL_ERR_WOULD_BLOCK;
  h2_web_serial_scan_js((uintptr_t)platform, state->scan_token);
  const h2_libco_result_t wait_result = h2_libco_wait(
      platform->executor, (uintptr_t)&state->scan_pending,
      H2_WEB_SERIAL_OPERATION_TIMEOUT_MS);
  if (wait_result != H2_LIBCO_WOKEN || !state->scan_done) {
    ++state->scan_token;
    state->scan_pending = false;
    free(state->scan_ports);
    state->scan_ports = NULL;
    state->scan_count = 0u;
    return wait_result == H2_LIBCO_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                               : H2_PAL_ERR_INVALID_STATE;
  }
  state->scan_pending = false;
  if (state->scan_result != H2_PAL_OK) {
    free(state->scan_ports);
    state->scan_ports = NULL;
    state->scan_count = 0u;
    return state->scan_result;
  }
  h2_pal_serial_host_snapshot_t *snapshot = calloc(1u, sizeof(*snapshot));
  if (snapshot == NULL) {
    free(state->scan_ports);
    state->scan_ports = NULL;
    state->scan_count = 0u;
    return H2_PAL_ERR_NO_MEMORY;
  }
  snapshot->ports = state->scan_ports;
  snapshot->count = state->scan_count;
  state->scan_ports = NULL;
  state->scan_count = 0u;
  *out_snapshot = snapshot;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_snapshot_count(
    void *user, const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
  (void)user;
  if (snapshot == NULL || out_count == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_count = snapshot->count;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_snapshot_get(
    void *user, const h2_pal_serial_host_snapshot_t *snapshot, size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
  (void)user;
  if (snapshot == NULL || out_info == NULL || index >= snapshot->count) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_info = snapshot->ports[index];
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_snapshot_destroy(
    void *user, h2_pal_serial_host_snapshot_t **inout_snapshot) {
  (void)user;
  if (inout_snapshot == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (*inout_snapshot != NULL) {
    free((*inout_snapshot)->ports);
    free(*inout_snapshot);
    *inout_snapshot = NULL;
  }
  return H2_PAL_OK;
}

static int h2_web_serial_port_number(const char *port_id,
                                     unsigned int *out_number) {
  static const char prefix[] = "web-serial-";
  if (port_id == NULL || out_number == NULL ||
      strncmp(port_id, prefix, sizeof(prefix) - 1u) != 0) {
    return 0;
  }
  const char *digits = port_id + sizeof(prefix) - 1u;
  char *end = NULL;
  errno = 0;
  const unsigned long value = strtoul(digits, &end, 10);
  if (errno != 0 || end == digits || *end != '\0' || value > UINT_MAX) {
    return 0;
  }
  *out_number = (unsigned int)value;
  return 1;
}

static h2_pal_result_t h2_web_serial_config_validate(
    const h2_pal_uart_io_stream_config_t *config) {
  if (config == NULL || config->baud_rate == 0u || config->data_bits < 5u ||
      config->data_bits > 8u ||
      (config->stop_bits != 1u && config->stop_bits != 2u) ||
      (config->parity != H2_PAL_UART_PARITY_NONE &&
       config->parity != H2_PAL_UART_PARITY_EVEN &&
       config->parity != H2_PAL_UART_PARITY_ODD) ||
      (config->flow_control & ~H2_PAL_UART_FLOW_CONTROL_RTS_CTS) != 0u ||
      config->rx_buffer_size == 0u || config->tx_buffer_size == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->data_bits < 7u ||
      (config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE &&
       config->flow_control != H2_PAL_UART_FLOW_CONTROL_RTS_CTS) ||
      config->rx_buffer_size > 16777216u) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_open(
    void *user, const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
  h2_web_platform_t *platform = user;
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  unsigned int port_number = 0u;
  if (out_session != NULL) {
    *out_session = NULL;
  }
  if (state == NULL || port_id == NULL || config == NULL ||
      out_session == NULL || !h2_web_serial_port_number(port_id, &port_number)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state->shutting_down) return H2_PAL_ERR_CLOSED;
  const h2_pal_result_t config_result =
      h2_web_serial_config_validate(config);
  if (config_result != H2_PAL_OK) {
    return config_result;
  }
  h2_pal_serial_host_session_t *session = calloc(1u, sizeof(*session));
  if (session == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  session->owner = platform;
  session->config = *config;
  session->next = state->sessions;
  state->sessions = session;
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_open_js((uintptr_t)platform, (uintptr_t)session, token,
                        port_number, config->baud_rate, config->data_bits,
                        config->stop_bits, config->parity,
                        config->flow_control, config->rx_buffer_size);
  const h2_pal_result_t result = h2_web_serial_wait(
      session, H2_WEB_SERIAL_OPERATION_TIMEOUT_MS);
  if (result != H2_PAL_OK) {
    state->sessions = session->next;
    h2_web_serial_force_close_js((uintptr_t)session);
    free(session);
    return result;
  }
  session->opened = true;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_stream_configure(
    void *user, const h2_pal_uart_io_stream_config_t *config) {
  h2_pal_serial_host_session_t *session = user;
  if (config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (session == NULL || !session->opened) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_web_serial_state_t *state = h2_web_serial_state(session->owner);
  if (state == NULL || state->shutting_down) return H2_PAL_ERR_CLOSED;
  const h2_pal_result_t config_result =
      h2_web_serial_config_validate(config);
  if (config_result != H2_PAL_OK) {
    return config_result;
  }
  return config->baud_rate == session->config.baud_rate &&
                 config->data_bits == session->config.data_bits &&
                 config->stop_bits == session->config.stop_bits &&
                 config->parity == session->config.parity &&
                 config->flow_control == session->config.flow_control &&
                 config->rx_buffer_size == session->config.rx_buffer_size &&
                 config->tx_buffer_size == session->config.tx_buffer_size
             ? H2_PAL_OK
             : H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_web_serial_stream_read(
    void *user, void *buffer, size_t length, size_t *out_read,
    uint32_t timeout_ms) {
  h2_pal_serial_host_session_t *session = user;
  if (out_read != NULL) {
    *out_read = 0u;
  }
  if (session == NULL || buffer == NULL || out_read == NULL || length == 0u ||
      !session->opened || session->closing || session->operation.pending) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_web_serial_session_shutting_down(session)) {
    return H2_PAL_ERR_CLOSED;
  }
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_read_js((uintptr_t)session->owner, (uintptr_t)session, token,
                        (uintptr_t)buffer, length, timeout_ms);
  const h2_pal_result_t result = h2_web_serial_wait(session, timeout_ms);
  if (result == H2_PAL_OK) {
    *out_read = session->operation.count;
  }
  return result;
}

static h2_pal_result_t h2_web_serial_stream_write(
    void *user, const void *buffer, size_t length, size_t *out_written,
    uint32_t timeout_ms) {
  h2_pal_serial_host_session_t *session = user;
  if (out_written != NULL) {
    *out_written = 0u;
  }
  if (session == NULL || buffer == NULL || out_written == NULL ||
      length == 0u || !session->opened || session->closing ||
      session->operation.pending) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_web_serial_session_shutting_down(session)) {
    return H2_PAL_ERR_CLOSED;
  }
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_write_js((uintptr_t)session->owner, (uintptr_t)session, token,
                         (uintptr_t)buffer, length, timeout_ms);
  const h2_pal_result_t result = h2_web_serial_wait(session, timeout_ms);
  if (result == H2_PAL_OK) {
    *out_written = session->operation.count;
  }
  return result;
}

static h2_pal_result_t h2_web_serial_stream_flush(void *user) {
  h2_pal_serial_host_session_t *session = user;
  if (session == NULL || !session->opened || session->closing ||
      session->operation.pending) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (h2_web_serial_session_shutting_down(session)) {
    return H2_PAL_ERR_CLOSED;
  }
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_flush_js((uintptr_t)session->owner, (uintptr_t)session, token);
  return h2_web_serial_wait(session, H2_PAL_SERIAL_HOST_FLUSH_TIMEOUT_MS);
}

static h2_pal_result_t h2_web_serial_session_stream(
    void *user, h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
  if (out_stream != NULL) {
    *out_stream = NULL;
  }
  if (user == NULL || session == NULL || session->owner != user ||
      out_stream == NULL || !session->opened || session->closing) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_web_serial_session_shutting_down(session)) {
    return H2_PAL_ERR_CLOSED;
  }
  static const h2_pal_uart_io_stream_vtable_t vtable = {
      .configure = h2_web_serial_stream_configure,
      .read = h2_web_serial_stream_read,
      .write = h2_web_serial_stream_write,
      .flush = h2_web_serial_stream_flush,
  };
  session->stream = (h2_pal_uart_io_stream_api_t){
      .user = session,
      .vtable = &vtable,
  };
  *out_stream = &session->stream;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_serial_set_control_lines(
    void *user, h2_pal_serial_host_session_t *session, uint32_t line_mask,
    uint32_t asserted_lines) {
  if (user == NULL || session == NULL || session->owner != user ||
      !session->opened || session->closing || session->operation.pending) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (h2_web_serial_session_shutting_down(session)) {
    return H2_PAL_ERR_CLOSED;
  }
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_signals_js(
      (uintptr_t)session->owner, (uintptr_t)session, token,
      (line_mask & H2_PAL_SERIAL_HOST_CONTROL_DTR) != 0u,
      (asserted_lines & H2_PAL_SERIAL_HOST_CONTROL_DTR) != 0u,
      (line_mask & H2_PAL_SERIAL_HOST_CONTROL_RTS) != 0u,
      (asserted_lines & H2_PAL_SERIAL_HOST_CONTROL_RTS) != 0u);
  return h2_web_serial_wait(session, H2_WEB_SERIAL_OPERATION_TIMEOUT_MS);
}

static h2_pal_result_t h2_web_serial_get_control_lines(
    void *user, h2_pal_serial_host_session_t *session,
    uint32_t *out_asserted_lines) {
  (void)user;
  (void)session;
  if (out_asserted_lines != NULL) {
    *out_asserted_lines = 0u;
  }
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_web_serial_close(
    void *user, h2_pal_serial_host_session_t **inout_session) {
  h2_web_platform_t *platform = user;
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL || inout_session == NULL || *inout_session == NULL ||
      (*inout_session)->owner != platform) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_serial_host_session_t *session = *inout_session;
  if (session->closing) {
    return H2_PAL_ERR_BUSY;
  }
  if (session->operation.pending) {
    const h2_libco_result_t wait_result = h2_libco_wait(
        platform->executor, (uintptr_t)&session->closing,
        H2_WEB_SERIAL_OPERATION_TIMEOUT_MS);
    if (session->operation.pending) {
      return wait_result == H2_LIBCO_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                                 : H2_PAL_ERR_BUSY;
    }
  }
  if (state->shutting_down) {
    h2_pal_serial_host_session_t **cursor = &state->sessions;
    while (*cursor != NULL && *cursor != session) {
      cursor = &(*cursor)->next;
    }
    if (*cursor == session) *cursor = session->next;
    ++session->operation.token;
    h2_web_serial_force_close_js((uintptr_t)session);
    session->opened = false;
    free(session);
    *inout_session = NULL;
    return H2_PAL_OK;
  }
  session->closing = true;
  const uint64_t token = h2_web_serial_begin(session);
  h2_web_serial_close_js((uintptr_t)platform, (uintptr_t)session, token);
  const h2_pal_result_t result = h2_web_serial_wait(
      session, H2_WEB_SERIAL_OPERATION_TIMEOUT_MS);
  if (result != H2_PAL_OK) {
    session->closing = false;
    return result;
  }
  h2_pal_serial_host_session_t **cursor = &state->sessions;
  while (*cursor != NULL && *cursor != session) {
    cursor = &(*cursor)->next;
  }
  if (*cursor == session) {
    *cursor = session->next;
  }
  session->opened = false;
  free(session);
  *inout_session = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_platform_serial_init(h2_web_platform_t *platform) {
  static const h2_pal_serial_host_vtable_t vtable = {
      .scan = h2_web_serial_scan,
      .snapshot_count = h2_web_serial_snapshot_count,
      .snapshot_get = h2_web_serial_snapshot_get,
      .snapshot_destroy = h2_web_serial_snapshot_destroy,
      .open = h2_web_serial_open,
      .session_stream = h2_web_serial_session_stream,
      .set_control_lines = h2_web_serial_set_control_lines,
      .get_control_lines = h2_web_serial_get_control_lines,
      .close = h2_web_serial_close,
  };
  h2_web_serial_state_t *state = calloc(1u, sizeof(*state));
  if (state == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  state->owner = platform;
  state->authorization_result = H2_PAL_ERR_WOULD_BLOCK;
  state->forget_result = H2_PAL_ERR_WOULD_BLOCK;
  platform->serial_state = state;
  h2_web_serial_platform_register_js((uintptr_t)platform);
  platform->serial_api = (h2_pal_serial_host_api_t){
      .user = platform,
      .vtable = &vtable,
  };
  return H2_PAL_OK;
}

void h2_web_platform_serial_deinit(h2_web_platform_t *platform) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL) {
    return;
  }
  h2_web_serial_platform_unregister_js((uintptr_t)platform);
  ++state->scan_token;
  ++state->authorization_token;
  ++state->forget_token;
  free(state->scan_ports);
  while (state->sessions != NULL) {
    h2_pal_serial_host_session_t *session = state->sessions;
    state->sessions = session->next;
    ++session->operation.token;
    h2_web_serial_force_close_js((uintptr_t)session);
    free(session);
  }
  free(state);
  platform->serial_state = NULL;
  platform->serial_api = (h2_pal_serial_host_api_t){0};
}

h2_pal_result_t
h2_web_platform_serial_request_port(h2_web_platform_t *platform) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL || platform->shutting_down || state->shutting_down) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (state->authorization_pending) {
    return H2_PAL_ERR_BUSY;
  }
  ++state->authorization_token;
  state->authorization_pending = true;
  state->authorization_terminal = false;
  state->authorization_result = H2_PAL_ERR_WOULD_BLOCK;
  state->authorized_port_id[0] = '\0';
  h2_web_serial_request_port_js((uintptr_t)platform,
                                state->authorization_token);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_web_platform_serial_shutdown(h2_web_platform_t *platform) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (state->shutting_down) return state->shutdown_result;
  const bool had_active_session = state->sessions != NULL;
  state->shutting_down = true;
  state->shutdown_result = had_active_session
      ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
  ++state->authorization_token;
  state->authorization_pending = false;
  state->authorization_terminal = true;
  state->authorization_result = H2_PAL_ERR_CLOSED;
  state->authorized_port_id[0] = '\0';
  ++state->forget_token;
  state->forget_pending = false;
  state->forget_terminal = true;
  state->forget_result = H2_PAL_ERR_CLOSED;
  if (state->scan_pending) {
    ++state->scan_token;
    state->scan_result = H2_PAL_ERR_CLOSED;
    state->scan_done = true;
    state->scan_wake_recorded = false;
  }
  for (h2_pal_serial_host_session_t *session = state->sessions;
       session != NULL; session = session->next) {
    ++session->operation.token;
    if (session->operation.pending) {
      session->operation.result = H2_PAL_ERR_CLOSED;
      session->operation.count = 0u;
      session->operation.done = true;
      session->operation.wake_recorded = false;
    }
    h2_web_serial_force_close_js((uintptr_t)session);
  }
  h2_web_platform_request_pump(platform, 0u);
  return state->shutdown_result;
}

h2_pal_result_t h2_web_platform_serial_authorization(
    h2_web_platform_t *platform, char *out_port_id, size_t out_size) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (out_port_id != NULL && out_size != 0u) {
    out_port_id[0] = '\0';
  }
  if (state == NULL || out_port_id == NULL || out_size == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state->authorization_pending || !state->authorization_terminal) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (state->authorization_result != H2_PAL_OK) {
    return state->authorization_result;
  }
  const size_t length = strlen(state->authorized_port_id);
  if (length + 1u > out_size) {
    return H2_PAL_ERR_NO_SPACE;
  }
  memcpy(out_port_id, state->authorized_port_id, length + 1u);
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_platform_serial_forget_port(
    h2_web_platform_t *platform, const char *port_id) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  unsigned int port_number = 0u;
  if (state == NULL || platform->shutting_down || state->shutting_down) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (!h2_web_serial_port_number(port_id, &port_number)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state->forget_pending) {
    return H2_PAL_ERR_BUSY;
  }
  ++state->forget_token;
  state->forget_pending = true;
  state->forget_terminal = false;
  state->forget_result = H2_PAL_ERR_WOULD_BLOCK;
  h2_web_serial_forget_port_js((uintptr_t)platform, state->forget_token,
                               port_number);
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_platform_serial_forget_result(
    h2_web_platform_t *platform) {
  h2_web_serial_state_t *state = h2_web_serial_state(platform);
  if (state == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (state->forget_pending || !state->forget_terminal) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return state->forget_result;
}
