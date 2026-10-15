# O4 system-event contract decision

**Decision needed; no provider or public-header behavior changed.** The references disagree, so there is no single behavior to port to JieLi.

| Question | Darwin desktop | ESP |
| --- | --- | --- |
| Post execution | Synchronous snapshot fanout; returns handler errors | Copies payload into an envelope and queues through `esp_event_post`; returns queue admission result |
| `timeout_ms` | Explicitly ignored, including mutex wait and handler runtime | Converted to queue-wait ticks; does not bound handler completion |
| Unsubscribe | Marks inactive and waits for all reserved `in_flight` callbacks | Delegates to SDK unregister under event-loop synchronization |
| Reentrant unsubscribe | Self-unsubscribe deadlocks waiting for its own reserved callback; removing a later callback in the same snapshot can also deadlock | SDK uses recursive dispatch locking and marks handlers unregistered for deferred removal when needed |

Sources: `libs/pal/providers/darwin/pal_core/src/h2_darwin_system_event.c`, functions `h2_darwin_system_event_post` and `h2_darwin_system_event_unsubscribe`; `native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_system_event.c`, post/unsubscribe. Local pinned ESP-IDF revision `662a3be354759d9487bf4b1a629fadb766cb1800`, `components/esp_event/esp_event.c`, dispatch loop, `esp_event_handler_unregister_with_internal` (line 859) and `find_and_unregister_handler` (line 440). The SDK recursive path permits retirement from the current dispatch rather than waiting for that callback to return. This does not claim every cross-provider lifetime corner is already safe.

Options:

1. Standardize asynchronous owned-payload queue admission, with `post(timeout_ms)` bounding admission only. External unsubscribe quiesces callbacks; unsubscribe from a handler retires subscriptions without waiting on the current dispatch. This follows ESP's execution model and requires desktop/JieLi changes plus consumer audits.
2. Standardize synchronous dispatch and propagate handler results. Define the timeout as dispatch admission only; handlers may exceed it. Require the same reentrant retirement rule and external quiescence, replacing Darwin's self-deadlocking wait and ESP's queue semantics. This requires ESP changes and audits of callers depending on asynchronous delivery.

Recommendation: option 1, because it provides an enforceable queue-admission budget for SDK event producers and explicit payload ownership. The maintainer must choose before changing the public contract or providers. Neither option may hold a registry lock across subscribers. Generation wrap must be specified with the chosen dispatch snapshot rule (for example identity/epoch retention until all dispatches retire), tested at wrap, and applied consistently; silently imposing a JieLi-only generation ceiling is not a contract decision.

Existing JieLi subscription-user lifetime, bounded-post and generation-wrap items remain unresolved pending that cross-platform decision. Hardware cannot establish the missing API contract, and no new behavior test is claimed for this documentation-only decision record.
