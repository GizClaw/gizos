# Console deadline follow-up — 2026-09-14

This is incremental O5 evidence. UART admission, physical submission and DMA staging ownership are repaired; Loader/App KCP output now shares command budgets; USB native submission uses the bounded short-packet path audited below. Final-source lifecycle acceptance remains pending.

## Pinned UART facts

SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu/wl82/liba/cpu.a`, decoded with the pinned compiler to `/tmp/jieli-pal-review-sdk/console/`:

- `spec_uart.c.o:uart_dev_write` takes the device writer mutex and calls `uartx_write`; `uart_dev_read` directly invokes its read operation. The board console is the sole linked owner opening/writing `uart1`; the SDK printf drain uses the board sink.
- `uartx_init` sets CON0 TX-interrupt-enable bit 2 unless platform `disable_tx_irq` is set. With TX IRQ enabled, `uartx_write` starts DMA and waits on `send_sem` with timeout zero (forever).
- The SDK's supported non-IRQ path waits for prior TX-done bit 15, acknowledges completion through bit 13, then programs TXADR/TXCNT and returns. It still borrows the supplied bytes for DMA. `uartx_isr` clears TX-done only when TX IRQ is enabled; RX interrupts therefore leave the completion predicate intact in this mode.

The fixed board task/platform policy disables TX IRQ, preserving RX DMA and interrupts. The console owns an aligned staging buffer until the TX-done predicate is observed, including after a caller deadline expires. It checks that predicate before any later copy or SDK write, so the SDK does not enter its old-transfer wait; exclusive console ownership keeps its internal writer mutex uncontended. No protocol or diagnostic writer owns another UART handle.

## Deadline behavior

Nonblocking reads use immediate mutex admission. Writes begin their budget before lock acquisition, use immediate admission for zero timeout, and use only whole remaining ticks when waiting in the SDK mutex queue. A sub-tick remainder is not rounded into another blocking tick. The same original budget bounds subsequent DMA completion waits and chunk submissions.

A positive short return reports an accepted prefix. DMA may still own its copied staging bytes after a timeout, and the next writer cannot overwrite them until completion. A zero-timeout call can accept an immediately available chunk without waiting for its completion. A call that accepts no bytes returns TIMEOUT on budget exhaustion. Normal finite writes wait for their final DMA completion when the budget permits. Short SDK progress is preserved; an SDK zero/error before accepting bytes returns IO.

## Host evidence

`//tools/bazel:jieli_console_deadlines_test` compiles the actual console source and checks the platform mode. Against `10399048`, six scenarios fail with strict Clang/GCC (also with TSan enabled): a read waiting behind a writer, zero-timeout rounding, a physical call exceeding the budget, and the asynchronous-mode staging/backpressure requirements including a real pthread holding the writer mutex. After repair seven scenarios pass: those six plus short native writes and zero-write error propagation. Fake DMA retains the actual staging pointer and validates its bytes at completion. A permanently stalled transfer times out later writers without losing or overwriting that buffer.

The existing `jieli_uart_console_test` is now registered with host-only compatibility. All five original regression tests pass with strict Clang/GCC, retaining startup/unwind/retry, complete and short writes, RX errors, display-size cases, and early App logging. Its display loop's existing unsigned comparison is made explicit without changing the integer conversion or behavior. No warning suppression is needed.

Logs: `/tmp/jieli-console-expanded-before.log`, `/tmp/jieli-console-expanded-gcc-before.log`, `/tmp/jieli-console-deadlines-after.log`, `/tmp/jieli-console-deadlines-gcc-after.log`, `/tmp/jieli-console-regression.log`, `/tmp/jieli-console-regression-gcc.log`. Both host registrations carry Linux/macOS/default-incompatible selects; iOS simulator analysis passes (`/tmp/jieli-console-ios.log`).

## USB native audit

The SDK `apps/common/usb/device/cdc.c:cdc_write_data` takes an indefinite internal mutex. `cpu.a:usb_phy.c.o:usb_g_bulk_write` delegates to `usb.c.o:usb_g_ep_write`, which polls TX-ready with a native deadline based on `jiffies + 200 + 2 * packet_count` (or 20 ticks before endpoint allocation), independent of a PAL caller's budget. The pre-existing board patch reduced the CDC data-mutex wait to one tick, but that still exceeded zero and sub-tick budgets and left the lower-level TX wait intact. Checking elapsed time only after that call was insufficient. USB is not enabled as the active transport on the current UART-owned board layout, so no USB hardware deadline result is claimed.

## Hardware and capture investigation

The final UART candidate package `558b2a252b6b50ee67b242c03735541e78fddeac009aae7b1e7629ae9d7b17e4`, image `c69e69e566bc02922f883aaa4fc44c2c532a877cba0f86409ace8a899309c0f0`, passed Filesystem 13, all eight Core cases and offline Wi-Fi 27 (10/10) in `o5-host-raw/1/pal.log`. Core operation tracing was disabled. UART installation, App status and return to the valid Loader succeeded. This is incremental App acceptance; the final-source Loader and lifecycle suites remain pending.

Earlier monitor captures omitted Filesystem 13 even though their aggregate was 10/10. A diagnostic App reported the preceding write's return count and showed the omitted line had been fully accepted. A separate raw serial capture showed complete repeated ledgers. Finally, a temporary host-only serial-read recorder captured the exact bytes before protocol/text filtering: the omitted Filesystem lines were intact, immediately following CRC-valid H2IKCP frames. Thus the missing monitor lines do not demonstrate UART DMA loss. The CLI text sink discards an entire line after binary contamination; the precise reconnect/filter path responsible remains an independent monitor issue. The recorder and diagnostic App edits were restored, and the ordinary CLI rebuilt. No timeout or retry was increased.

An experimental post-submit CPU synchronization instruction did not prevent the monitor omission and was removed. Its package is not the accepted candidate. The [machine-readable capture evidence](../2026-09-15/pal-buffered-poll.md#retained-acceptance-facts-pal-console-hardware) records package identity and raw-frame boundaries. Raw files are `o5-host-raw/1/pal.log`, `/tmp/jieli-o5-host-raw.bin`, and `o5-csync/1/raw-uart.log` under the local diagnostic evidence directory (the last is the separate experimental package).

Native Loader, PAL and display builds passed (64.193 seconds). Strict Clang/TSan and GCC results are `/tmp/jieli-o5-uart-verified-host.log` and `/tmp/jieli-o5-uart-verified-gcc.log`. P1 remained Loader image `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; P2 contains the accepted unconfirmed PAL App, with its stage retained.

## Command-to-KCP budget propagation

Loader and App previously replaced zero command-write timeout with five seconds, and KCP used its fixed five-second physical timeout for each output frame. Time already spent waiting for the send window or reading input did not reduce that timeout. The initial flush also ran before the flush budget began.

The command task now scopes the original deadline around read/write/flush. KCP output and session-control replies receive the remaining budget. Multiple frames consume one budget; an exhausted finite deadline rejects further submission. Zero timeout remains immediate admission. Flush keeps its existing backlog estimate and cap, but includes its initial physical output. The scope ends on errors as well as success. No shared library configuration switch or alternate behavior mode was added.

`//tools/bazel:jieli_command_deadlines_test` compiles both real transport structures, stream configurations, control writers and command implementations. Nine scenarios fail on each original provider (18 failures): finite, zero, send-window wait, zero with a full window, multiple frames, read-triggered output, control reply, initial flush and a fully consumed window budget. All pass with strict Clang/GCC. Logs are `/tmp/jieli-command-deadlines-expanded-before.log`, `/tmp/jieli-command-deadlines-expanded-gcc-before.log`, `/tmp/jieli-command-deadlines-final-after.log` and `/tmp/jieli-command-deadlines-final-gcc-after.log`. UART regressions also pass. Native Loader, PAL and display builds pass (68.662 seconds). The command-budget PAL package `8f8baff550fd61908be21e9e6046d3cecfa97a0f421bde9f1e1da42a946adf8a`, image `989d692e899728eb4ec27a19f7a19ffa9d316bba713d2c3a35e682a1d38b7038`, passes Filesystem 13, all eight Core cases and offline Wi-Fi 27 (10/10), with complete monitor and raw UART ledgers in `o5-command-budget/1/`. UART installation, App status and return to unchanged valid P1 succeed. Final-source Loader/lifecycle acceptance remains pending.

The subsequent bounded CDC patch completes the physical USB deadline path described next.

## Bounded CDC submission

The applied `bounded_cdc_write.patch` changes the layout's CDC writer to immediate data-mutex admission and one available short packet (at most native maximum packet size minus one byte). It checks native DMA allocation and TXCSR bit 0 before submission. Short packets avoid the old trailing zero-length packet call, which could wait for the preceding packet indefinitely relative to the caller budget. The SDK copies submitted bytes into its own endpoint DMA allocation before returning, so caller stack storage is no longer borrowed by that call.

Pinned `cpu.a:usb_phy.c.o:usb_g_bulk_write` directly calls `usb.c.o:usb_g_ep_write`. On the ready, one-packet path the latter copies bytes, synchronizes, programs the DMA address and calls `usb_g_tx_flushfifo`; that helper writes the count and TXCSR ready bit. These paths contain no subscriber or synchronous callback. The SDK data mutex excludes all CDC task writers. `usb_g_ep_config` resets TXCSR with 72 then 16384, neither setting ready bit 0, and reuses the endpoint buffer. The layout's project source list excludes `task_pc.c`, whose `usb_pause` performs class teardown; the board starts CDC once and exposes no runtime class teardown. Thus this implementation relies on the existing boot-lifetime CDC/DMA ownership, not an invented delay or a release/reopen recovery rule.

Loader/App physical writers now start the budget before outer-lock admission, use immediate admission for zero timeout, wait only whole remaining ticks, and bound each subsequent availability wait. A finite expired budget cannot submit another packet. Accepted short progress is retained in `out_written`. The two millisecond-to-tick helpers use division and remainder without overflowing `ms + 9`. SDK printf and diagnostic callers already consume returned prefixes; they use the same bounded CDC implementation. No second native writer or new configuration mode was introduced.

`//tools/bazel:jieli_usb_deadlines_test` extracts both actual physical writers and the CDC function selected by the firmware patch manifest. Nine scenarios fail on each original provider with the actual pre-existing one-tick CDC patch (18 failures): tick overflow, zero/finite outer-lock contention, busy TX, disconnected CDC, absent DMA, a real pthread holding the native mutex, full packet termination and short native writes. They pass with strict Clang/TSan and GCC. Logs: `/tmp/jieli-usb-deadlines-before.log`, `/tmp/jieli-usb-deadlines-gcc-before.log`, `/tmp/jieli-usb-deadlines-final-after.log`, `/tmp/jieli-usb-deadlines-final-gcc.log`. Native Loader/PAL/display builds pass (65.322 seconds). A patch preimage preparation error was caught and corrected by native build before acceptance; baseline tests were rerun against the correct patched SDK function.

USB hardware throughput, unplug/reconnect behavior and endpoint reset interleavings are **not covered on hardware** in the UART1-owned layout. Final-source UART/PAL/BLE lifecycle acceptance remains pending.
