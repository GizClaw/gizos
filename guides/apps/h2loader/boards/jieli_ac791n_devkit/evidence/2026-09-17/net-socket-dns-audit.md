# Socket concurrency and DNS lifecycle audit — 2026-09-17

Closes the O7 item of the [2026-09-14 PAL review](../2026-09-14/pal-review.md). Source is PR #457 (Issue #452) on top of `origin/main` `4fd6e947`; the pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. No PAL public header or other platform changed.

## What lwIP guarantees

The pinned `lwipopts.h` sets `LWIP_NETCONN_FULLDUPLEX 1`, `LWIP_NETCONN_SEM_PER_THREAD 1`, `LWIP_TCPIP_CORE_LOCKING 0`, `LWIP_SO_RCVTIMEO 1` and `LWIP_SO_SNDTIMEO 1`; `MEMP_NUM_NETCONN` is 55 and `LWIP_SOCKET_OFFSET` is 0.
`sockets.c:371–446` counts `fd_used` per socket so a descriptor is not freed under a concurrent user; `api_msg.c:1117–1160` aborts a blocking write or connect with `ERR_CLSD` on close and invalidates the mailboxes, a blocked `recv` returns `ERR_CONN` (`api_lib.c:628–636`), and a select whose socket vanished returns `EBADF` (`sockets.c:2160–2200`).
The `opt.h` description of `LWIP_NETCONN_FULLDUPLEX` limits this to one reader, one writer and one closer: `lwip_recv_tcp` manipulates `sock->lastdata` unprotected and a second writer sees `ERR_INPROGRESS` from `lwip_netconn_do_write`.
`SO_RCVTIMEO`, `SO_SNDTIMEO` and `FIONBIO` are netconn state read at each fetch (`api_lib.c:616`), so a second same-direction call rewrites what the first is blocked on, and a connect in progress makes a concurrent transfer return `EWOULDBLOCK` immediately.
`netdb.c:92–145` returns one static `hostent` for every thread because `LWIP_DNS_API_HOSTENT_STORAGE` is 0; `netconn_gethostbyname_addrtype` uses the per-thread semaphore.
`dns.c:1084–1160` retries a query with `tmr = 1, 1, 2, 3` seconds per configured server and then calls the found callback once with NULL; the callback also runs once on an answer, there is no cancel, and `dns_gethostbyname_addrtype` answers IP literals, `localhost` and cached names synchronously.

## Where the stack lives

`wifi_connect.c.o:WIFI_state_on_hdl` enters with `wifi_module_init` and `Init_LwIP(1)` (`tcpip_init` once, `netif_add`) and exits with `wifi_module_remove` and `Unint_LwIP(1)`, both inside `wifi_on()`/`wifi_off()` under `network_hsm_mtx`.
`port/LwIP.c:1439–1486` tears down with `netif_set_down`, `sys_timeouts_uninit`, `netif_remove` and `tcpip_uninit`; `api/tcpip.c:678–682` only clears `tcpip_task_running`, and `tcpip.c:128–155` keeps the old thread blocked on the old mailbox until the next message, which it drops.
After `wifi_off()` every `tcpip_send_msg_wait_sem` caller (socket, connect, send, close, `netconn_tcp_recvd`, synchronous DNS) waits forever, `tcpip_try_callback` posts into a queue nobody drains, and the next `tcpip_init` recycles netconn memory through `memp_init` while `sockets[]` keeps stale descriptors marked used.
`wifi_enter_sta_mode` and `wifi_module_exit_sta_mode` re-create the driver (`wifi_module_remove`) but not lwIP, so connect and disconnect-to-STA transitions keep sockets alive.

## Provider additions

Each descriptor has busy bits (`RECV`, `SEND`, `CONNECT`) and a stack generation: overlapping same-direction calls and connect against any transfer return `H2_PAL_ERR_BUSY`, a descriptor from an older generation returns `H2_PAL_ERR_UNAVAILABLE` and its close is a no-op.
A stack gate shared through `h2_jieli_ac791n_devkit_network.h` makes every lwIP-reaching call return `H2_PAL_ERR_UNAVAILABLE` before `h2_jieli_net_stack_started` and after `h2_jieli_net_stack_stopping`; stopping waits for in-flight native calls to return before `wifi_off`, and stopped settles pending resolvers with `H2_PAL_ERR_UNAVAILABLE`.
Resolvers live in a four-slot registry and lwIP receives a monotonically increasing nonzero callback ID as its opaque context instead of the resolver pointer; a callback is matched by ID against the registry only, so a settled, reaped or address-reused resolver is never dereferenced, and resolvers settled by a stop stay allocated in a graveyard until the next successful start releases their lwIP reference; a failed `wifi_off` leaves the stack unavailable without settling anything; `resolve_addr` uses `netconn_gethostbyname_addrtype` with caller-owned storage.

## Host validation

`bazel test --config=macos_arm64 //tools/bazel:all` passes 99/99 with the new `jieli_net_concurrency_test` and `jieli_net_stack_lifecycle_test` and the updated DNS, timeout and Wi-Fi fixtures.
Against `main` `4fd6e947` the concurrency fixture's fake observes an overlapping same-direction call and the lifecycle fixture finds `gethostbyname` in `resolve_addr`.
The threaded fixtures pass under macOS `-fsanitize=thread` and Linux GCC in OrbStack; `bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all` passes.

## Native build and board run

The Loader and PAL packages were built from `ff47a7fa` in OrbStack `embed-zig-noble-amd64` with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-` after sourcing the firmware devenv; the PAL package is `26cb8435057457cdd103d65a02f56fd5d737b1836a9650b2f38957a1f9318518` (893091 bytes) with image `945f4345c24f01adcc8bd532ecc1603657dda145b48460bd68d6245acd9c1ba7` (904269 bytes), and the Loader package `506ccc62ff459d3bde7bb154525091d51aabed32a5bc681c1552767beb0b8e77` was built but not installed.
On UID `d879349abc9f` (`/dev/cu.usbserial-20131240`, 460800) the board started on the main P1 Loader `ed7d71a6…` with `last_result=0`; the PAL package was sent through the UART Loader in 30 s (`H2_LOADER_SEND result=OK`, checksum matching), independent status showed `stage_valid=1` with that package and image, and `reboot upgrade --monitor` was stopped by SIGINT after 110 s (exit 124 from `timeout`, not a board fact).
The capture holds `H2_PAL_E2E phase=runtime result=0`, `suite=64 case=13 result=0`, Core cases 1, 2, 3, 4, 5, 6, 10 and 11 all `result=0`, `suite=32 case=27 result=0` and `H2_PAL_E2E result=0 passed=10 failed=0`; Wi-Fi 27 runs `sta_disconnect` through the new stopping/stopped hooks before the netif snapshot.
Independent status while the App ran showed `active_role=app`, `running_partition=2`, `partition_2_image_checksum=945f4345…`, `last_result=0`; `reboot loader` was accepted and the following status showed `active_role=loader`, `running_partition=1`, `active_checksum=ed7d71a6…`, `last_result=0`, and the stage was cleared with `stage abort` at the end of the session's board window.

## Boundaries

Descriptors left open across a Wi-Fi stop are unrecoverable by design; the contract requires closing every socket and resolver before disconnect or AP stop. Connected TCP/UDP traffic under concurrent callers was exercised only by host fixtures, not on hardware.
