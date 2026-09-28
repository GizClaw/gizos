#include "h2_web_main_thread.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The browser exposes one host-managed default path and a boolean
 * navigator.onLine signal. It hides addresses, gateway, DNS, MTU, MAC and the
 * physical link type, so the provider reports none of them. navigator.onLine
 * only says whether the host has some network: services may still be
 * unreachable, which only an actual Fetch/WebRTC attempt can decide.
 */

struct h2_pal_system_event_subscription {
  h2_pal_system_event_subscription_t *next;
  h2_pal_system_event_type_t type;
  h2_pal_system_event_handler_t handler;
  void *handler_user;
  uint64_t generation;
  size_t in_flight;
  size_t waiters;
  bool active;
};

struct h2_web_system_event_frame {
  h2_web_system_event_frame_t *next;
  h2_pal_system_event_subscription_t *subscription;
  h2_web_platform_t *platform;
};

static void
h2_web_system_event_reclaim(h2_web_platform_t *platform,
                            h2_pal_system_event_subscription_t *target) {
  if (target->active || target->in_flight != 0u || target->waiters != 0u)
    return;
  h2_pal_system_event_subscription_t **cursor =
      &platform->system_event_subscriptions;
  while (*cursor != NULL && *cursor != target)
    cursor = &(*cursor)->next;
  if (*cursor == NULL)
    return;
  *cursor = target->next;
  --platform->system_event_subscription_count;
  free(target);
}

static _Thread_local h2_web_system_event_frame_t *dispatch_stack;

static bool
h2_web_system_event_self(h2_web_platform_t *platform,
                         h2_pal_system_event_subscription_t *target) {
  for (h2_web_system_event_frame_t *frame = dispatch_stack; frame != NULL;
       frame = frame->next)
    if (frame->subscription == target && frame->platform == platform)
      return true;
  return false;
}

/* clang-format off */
EM_JS(void, h2_web_netif_install_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (platform_address) => {
  const navigator = globalThis.navigator;
  if (!navigator || typeof navigator.onLine !== 'boolean') return 0;
  const entries = Module['h2WebNetif'] ||= new Map();
  const onchange = () => {
    if (entries.get(platform_address) !== entry) return;
    Module['_h2_web_netif_changed'](platform_address);
  };
  const entry = {onchange};
  if (typeof globalThis.addEventListener === 'function') {
    globalThis.addEventListener('online', onchange);
    globalThis.addEventListener('offline', onchange);
  }
  entries.set(platform_address, entry);
  return 1;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_netif_uninstall_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (platform_address) => {
  const entries = Module['h2WebNetif'];
  const entry = entries && entries.get(platform_address);
  if (!entry) return;
  entries.delete(platform_address);
  if (typeof globalThis.removeEventListener === 'function') {
    globalThis.removeEventListener('online', entry.onchange);
    globalThis.removeEventListener('offline', entry.onchange);
  }
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_netif_online_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => {
  return globalThis.navigator && navigator.onLine === false ? 0 : 1;
});
});
/* clang-format on */

static bool h2_web_netif_online(h2_web_platform_t *platform) {
  return platform->netif_supported &&
         ((int)h2_web_main_call(h2_web_netif_online_js, NULL).i32) != 0;
}

static h2_pal_netif_ref_t h2_web_netif_ref(void) {
  h2_pal_netif_ref_t ref;
  memset(&ref, 0, sizeof(ref));
  ref.type = H2_PAL_NETIF_REF_NAME;
  ref.kind = H2_PAL_NETIF_KIND_HOST;
  memcpy(ref.name, H2_WEB_NETIF_NAME, sizeof(H2_WEB_NETIF_NAME));
  return ref;
}

static void h2_web_netif_fill_status(bool online,
                                     h2_pal_netif_status_t *out_status) {
  memset(out_status, 0, sizeof(*out_status));
  out_status->ref = h2_web_netif_ref();
  out_status->kind = H2_PAL_NETIF_KIND_HOST;
  // The host network stack always exists; LINK_UP follows navigator.onLine.
  out_status->flags = H2_PAL_NETIF_FLAG_UP;
  if (online) {
    out_status->flags |=
        H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
  }
}

static bool h2_web_netif_filter_matches(const h2_pal_netif_filter_t *filter) {
  return filter == NULL ||
         ((filter->kind == H2_PAL_NETIF_KIND_UNKNOWN ||
           filter->kind == H2_PAL_NETIF_KIND_HOST) &&
          (filter->name == NULL || filter->name[0] == '\0' ||
           strncmp(filter->name, H2_WEB_NETIF_NAME, H2_PAL_NETIF_NAME_MAX) ==
               0) &&
          filter->id == 0u);
}

/* Resolve a ref to the browser path; DEFAULT resolves only while online. */
static h2_pal_result_t h2_web_netif_resolve(h2_web_platform_t *platform,
                                            const h2_pal_netif_ref_t *ref,
                                            bool *out_online) {
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (!platform->netif_supported)
    return H2_PAL_ERR_UNSUPPORTED;
  const bool online = h2_web_netif_online(platform);
  *out_online = online;
  if (h2_pal_netif_ref_is_default(ref))
    return online ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
  if (ref->type == H2_PAL_NETIF_REF_NAME) {
    return memchr(ref->name, '\0', sizeof(ref->name)) != NULL &&
                   strcmp(ref->name, H2_WEB_NETIF_NAME) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_NOT_FOUND;
  }
  if (ref->type == H2_PAL_NETIF_REF_KIND)
    return ref->kind == H2_PAL_NETIF_KIND_HOST ? H2_PAL_OK
                                               : H2_PAL_ERR_NOT_FOUND;
  if (ref->type == H2_PAL_NETIF_REF_ID)
    return H2_PAL_ERR_NOT_FOUND;
  return H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t h2_web_netif_list(void *user,
                                         const h2_pal_netif_filter_t *filter,
                                         h2_pal_netif_list_fn on_netif,
                                         void *callback_user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (!platform->netif_supported)
    return H2_PAL_ERR_UNSUPPORTED;
  if (!h2_web_netif_filter_matches(filter))
    return H2_PAL_OK;
  h2_pal_netif_status_t status;
  h2_web_netif_fill_status(h2_web_netif_online(platform), &status);
  return on_netif(callback_user, &status.ref, &status) != 0 ? H2_PAL_EXIT
                                                            : H2_PAL_OK;
}

static h2_pal_result_t h2_web_netif_find(void *user,
                                         const h2_pal_netif_filter_t *filter,
                                         h2_pal_netif_ref_t *out_ref) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (!platform->netif_supported)
    return H2_PAL_ERR_UNSUPPORTED;
  if (!h2_web_netif_filter_matches(filter))
    return H2_PAL_ERR_NOT_FOUND;
  *out_ref = h2_web_netif_ref();
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_netif_get_status(void *user, const h2_pal_netif_ref_t *ref,
                        h2_pal_netif_status_t *out_status) {
  memset(out_status, 0, sizeof(*out_status));
  bool online = false;
  const h2_pal_result_t result = h2_web_netif_resolve(user, ref, &online);
  if (result != H2_PAL_OK)
    return result;
  h2_web_netif_fill_status(online, out_status);
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_netif_get_dns_servers(void *user, const h2_pal_netif_ref_t *ref,
                             h2_pal_netif_dns_server_t *out_servers,
                             size_t max_servers, size_t *out_count) {
  (void)out_servers;
  (void)max_servers;
  *out_count = 0u;
  bool online = false;
  const h2_pal_result_t result = h2_web_netif_resolve(user, ref, &online);
  // Browsers resolve names internally and never expose their resolvers.
  return result == H2_PAL_OK ? H2_PAL_ERR_UNSUPPORTED : result;
}

static h2_pal_result_t h2_web_netif_set_default(void *user,
                                                const h2_pal_netif_ref_t *ref) {
  bool online = false;
  const h2_pal_result_t result = h2_web_netif_resolve(user, ref, &online);
  // The browser owns routing; there is no other path to select.
  return result == H2_PAL_OK ? H2_PAL_ERR_UNSUPPORTED : result;
}

static const h2_pal_netif_vtable_t h2_web_netif_vtable = {
    .list = h2_web_netif_list,
    .find = h2_web_netif_find,
    .get_status = h2_web_netif_get_status,
    .get_dns_servers = h2_web_netif_get_dns_servers,
    .set_default = h2_web_netif_set_default,
};

static int h2_web_system_event_init(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  const bool online = h2_web_netif_online(platform);
  pthread_mutex_lock(&platform->event_mutex);
  if (platform->system_event_users++ == 0u) {
    platform->netif_online = online;
    platform->netif_dirty = false;
  }
  pthread_mutex_unlock(&platform->event_mutex);
  return H2_PAL_OK;
}

static void h2_web_system_event_deinit(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL)
    return;
  pthread_mutex_lock(&platform->event_mutex);
  if (platform->system_event_users != 0u)
    --platform->system_event_users;
  pthread_mutex_unlock(&platform->event_mutex);
}

static int h2_web_system_event_post(void *user,
                                    const h2_pal_system_event_t *event,
                                    uint32_t timeout_ms) {
  (void)timeout_ms;
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  const int validation = h2_pal_system_event_validate(event);
  if (validation != H2_PAL_OK)
    return validation;
  // Snapshot identities, not callbacks: retiring/reusing a slot must not admit
  // a newly registered subscriber into a post that preceded its registration.
  struct {
    h2_pal_system_event_subscription_t *subscription;
    uint64_t generation;
  } matches[H2_WEB_SYSTEM_EVENT_SUBSCRIPTION_MAX];
  pthread_mutex_lock(&platform->event_mutex);
  size_t count = 0u;
  for (h2_pal_system_event_subscription_t *subscription =
           platform->system_event_subscriptions;
       subscription != NULL && count < H2_WEB_SYSTEM_EVENT_SUBSCRIPTION_MAX;
       subscription = subscription->next) {
    if (subscription->active && subscription->type == event->type) {
      matches[count].subscription = subscription;
      matches[count++].generation = subscription->generation;
    }
  }
  ++platform->system_event_posts;
  int result = H2_PAL_OK;
  for (size_t index = 0u; index < count; ++index) {
    h2_pal_system_event_subscription_t *live = NULL;
    for (h2_pal_system_event_subscription_t *subscription =
             platform->system_event_subscriptions;
         subscription != NULL && live == NULL; subscription = subscription->next)
      if (subscription == matches[index].subscription && subscription->active &&
          subscription->generation == matches[index].generation)
        live = subscription;
    if (live == NULL)
      continue;
    ++live->in_flight;
    h2_web_system_event_frame_t frame = {
        .next = dispatch_stack, .subscription = live, .platform = platform};
    dispatch_stack = &frame;
    pthread_mutex_unlock(&platform->event_mutex);
    const int handler_result = live->handler(live->handler_user, event);
    pthread_mutex_lock(&platform->event_mutex);
    dispatch_stack = frame.next;
    --live->in_flight;
    pthread_cond_broadcast(&platform->event_changed);
    h2_web_system_event_reclaim(platform, live);
    if (result == H2_PAL_OK && handler_result != H2_PAL_OK)
      result = handler_result;
  }
  --platform->system_event_posts;
  pthread_cond_broadcast(&platform->event_changed);
  pthread_mutex_unlock(&platform->event_mutex);
  return result;
}

static int h2_web_system_event_subscribe(
    void *user, h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler, void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (handler == NULL || out_subscription == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&platform->event_mutex);
  if (platform->system_event_subscription_count >=
      H2_WEB_SYSTEM_EVENT_SUBSCRIPTION_MAX) {
    pthread_mutex_unlock(&platform->event_mutex);
    return H2_PAL_ERR_NO_SPACE;
  }
  h2_pal_system_event_subscription_t *subscription =
      calloc(1u, sizeof(*subscription));
  if (subscription == NULL) {
    pthread_mutex_unlock(&platform->event_mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  subscription->type = type;
  subscription->handler = handler;
  subscription->handler_user = handler_user;
  subscription->active = true;
  if (++platform->system_event_next_generation == 0u)
    ++platform->system_event_next_generation;
  subscription->generation = platform->system_event_next_generation;
  subscription->next = platform->system_event_subscriptions;
  platform->system_event_subscriptions = subscription;
  ++platform->system_event_subscription_count;
  *out_subscription = subscription;
  pthread_mutex_unlock(&platform->event_mutex);
  return H2_PAL_OK;
}

static void
h2_web_system_event_unsubscribe(void *user,
                                h2_pal_system_event_subscription_t *target) {
  h2_web_platform_t *platform = user;
  if (platform == NULL)
    return;
  // A synchronous PAL void-returning unsubscribe cannot report WOULD_BLOCK.
  // Its borrowed API is Worker-only; UI callers use the asynchronous entry.
  if (emscripten_is_main_runtime_thread()) {
    emscripten_err("SystemEvent unsubscribe requires a Worker; use unsubscribe_async on the browser main thread");
    abort();
  }
  pthread_mutex_lock(&platform->event_mutex);
  h2_pal_system_event_subscription_t **cursor =
      &platform->system_event_subscriptions;
  while (*cursor != NULL && *cursor != target)
    cursor = &(*cursor)->next;
  if (*cursor == NULL) {
    pthread_mutex_unlock(&platform->event_mutex);
    return;
  }
  target->active = false;
  if (h2_web_system_event_self(platform, target)) {
    pthread_mutex_unlock(&platform->event_mutex);
    return;
  }
  ++target->waiters;
  ++platform->system_event_unsubscribe_waiters;
  while (target->in_flight != 0u)
    pthread_cond_wait(&platform->event_changed, &platform->event_mutex);
  --target->waiters;
  --platform->system_event_unsubscribe_waiters;
  h2_web_system_event_reclaim(platform, target);
  pthread_mutex_unlock(&platform->event_mutex);
}

struct h2_web_event_unsubscribe_request {
  h2_web_event_unsubscribe_request_t *next;
  h2_pal_system_event_subscription_t *subscription;
  void (*completed)(void *);
  void *user;
};

h2_pal_result_t h2_web_platform_system_event_unsubscribe_async(
    h2_web_platform_t *platform, h2_pal_system_event_subscription_t *target,
    void (*completed)(void *), void *user) {
  if (platform == NULL || target == NULL || completed == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_event_unsubscribe_request_t *request = calloc(1u, sizeof(*request));
  if (request == NULL) return H2_PAL_ERR_NO_MEMORY;
  // Neither admission lock may block the browser. Holding both short-lived
  // locks pins the subscription identity before it can be retired or reused.
  if (pthread_mutex_trylock(&platform->pump_mutex) != 0) {
    free(request);
    return H2_PAL_ERR_BUSY;
  }
  if (pthread_mutex_trylock(&platform->event_mutex) != 0) {
    pthread_mutex_unlock(&platform->pump_mutex);
    free(request);
    return H2_PAL_ERR_BUSY;
  }
  h2_pal_result_t rc = H2_PAL_OK;
  h2_pal_system_event_subscription_t *live = platform->system_event_subscriptions;
  while (live != NULL && live != target) live = live->next;
  if (platform->shutting_down || platform->pump_stop)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (live == NULL)
    rc = H2_PAL_ERR_NOT_FOUND;
  else if (live->waiters != 0u)
    rc = H2_PAL_ERR_BUSY;
  if (rc == H2_PAL_OK) {
    live->active = false;
    ++live->waiters;
    ++platform->system_event_unsubscribe_waiters;
    request->subscription = live;
    request->completed = completed;
    request->user = user;
    if (platform->event_unsubscribe_tail != NULL)
      platform->event_unsubscribe_tail->next = request;
    else
      platform->event_unsubscribe_head = request;
    platform->event_unsubscribe_tail = request;
    platform->pump_scheduled = true;
    pthread_cond_signal(&platform->pump_changed);
  }
  pthread_mutex_unlock(&platform->event_mutex);
  pthread_mutex_unlock(&platform->pump_mutex);
  if (rc != H2_PAL_OK) free(request);
  return rc;
}

void h2_web_platform_event_retire(h2_web_platform_t *platform) {
  for (;;) {
    pthread_mutex_lock(&platform->pump_mutex);
    h2_web_event_unsubscribe_request_t *request = platform->event_unsubscribe_head;
    if (request != NULL) {
      platform->event_unsubscribe_head = request->next;
      if (platform->event_unsubscribe_head == NULL)
        platform->event_unsubscribe_tail = NULL;
    }
    pthread_mutex_unlock(&platform->pump_mutex);
    if (request == NULL) return;
    if (h2_web_system_event_self(platform, request->subscription)) {
      // A caller may pump from inside the callback being retired. Let the
      // owned Worker process it after this callback unwinds instead of waiting
      // for ourselves from a nested pump.
      pthread_mutex_lock(&platform->pump_mutex);
      request->next = platform->event_unsubscribe_head;
      platform->event_unsubscribe_head = request;
      if (platform->event_unsubscribe_tail == NULL)
        platform->event_unsubscribe_tail = request;
      platform->pump_scheduled = true;
      pthread_cond_signal(&platform->pump_changed);
      pthread_mutex_unlock(&platform->pump_mutex);
      return;
    }
    pthread_mutex_lock(&platform->event_mutex);
    h2_pal_system_event_subscription_t *target = request->subscription;
    while (target->in_flight != 0u)
      pthread_cond_wait(&platform->event_changed, &platform->event_mutex);
    --target->waiters;
    --platform->system_event_unsubscribe_waiters;
    h2_web_system_event_reclaim(platform, target);
    pthread_mutex_unlock(&platform->event_mutex);
    // platform->pumping pins the owner through the completion callback.
    request->completed(request->user);
    free(request);
  }
}

static const h2_pal_system_event_vtable_t h2_web_system_event_vtable = {
    .init = h2_web_system_event_init,
    .deinit = h2_web_system_event_deinit,
    .post = h2_web_system_event_post,
    .subscribe = h2_web_system_event_subscribe,
    .unsubscribe = h2_web_system_event_unsubscribe,
};

EMSCRIPTEN_KEEPALIVE void h2_web_netif_changed(uintptr_t platform_address) {
  h2_web_platform_t *platform = (h2_web_platform_t *)platform_address;
  if (platform == NULL || platform->shutting_down)
    return;
  // Browser callbacks only record the wake; the pump publishes the event.
  platform->netif_dirty = true;
  h2_web_platform_request_pump(platform, 0u);
}

void h2_web_platform_netif_poll(h2_web_platform_t *platform) {
  if (!atomic_exchange(&platform->netif_dirty, false))
    return;
  const bool online = h2_web_netif_online(platform);
  pthread_mutex_lock(&platform->event_mutex);
  if (platform->system_event_users == 0u || online == platform->netif_online) {
    platform->netif_online = online;
    pthread_mutex_unlock(&platform->event_mutex);
    return;
  }
  h2_pal_netif_default_changed_t change;
  memset(&change, 0, sizeof(change));
  if (platform->netif_online) {
    change.previous = h2_web_netif_ref();
    change.previous_valid = 1u;
  }
  if (online) {
    change.current = h2_web_netif_ref();
    change.current_valid = 1u;
  }
  platform->netif_online = online;
  pthread_mutex_unlock(&platform->event_mutex);
  const h2_pal_system_event_t event = {
      .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
      .timestamp_ms = (uint64_t)emscripten_get_now(),
      .payload = &change,
      .payload_size = sizeof(change),
  };
  const int result =
      h2_web_system_event_post(platform, &event, 0u);
  if (result != H2_PAL_OK) {
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "default-route event delivery failed rc=%d", result);
    (void)h2_pal_log_write(h2_web_platform_log_api(), H2_PAL_LOG_WARN,
                           "web_netif", message);
  }
}

void h2_web_platform_netif_init(h2_web_platform_t *platform) {
  platform->netif_api = (h2_pal_netif_api_t){
      .user = platform,
      .vtable = &h2_web_netif_vtable,
  };
  platform->system_event_api = (h2_pal_system_event_api_t){
      .user = platform,
      .vtable = &h2_web_system_event_vtable,
  };
  platform->netif_supported =
      ((int)h2_web_main_call(
           h2_web_netif_install_js,
           (const void *[]){&(uintptr_t){(uintptr_t)platform}})
           .i32) != 0;
  platform->netif_online = h2_web_netif_online(platform);
}

void h2_web_platform_netif_deinit(h2_web_platform_t *platform) {
  (void)h2_web_main_call(h2_web_netif_uninstall_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)platform}});
  while (platform->system_event_subscriptions != NULL) {
    h2_pal_system_event_subscription_t *subscription =
        platform->system_event_subscriptions;
    platform->system_event_subscriptions = subscription->next;
    free(subscription);
  }
  platform->system_event_subscription_count = 0u;
}
