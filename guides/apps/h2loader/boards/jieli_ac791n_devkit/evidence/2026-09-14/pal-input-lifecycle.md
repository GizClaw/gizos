# Display, touch and button lifecycle follow-up

The O6 changes serialize public display/touch operations using a nonwaiting atomic admission reservation. Contending or reentrant operations return BUSY. No PAL mutex or spinlock remains held across an SDK call. ADC registration similarly publishes ready only after successful initialization; concurrent first use returns BUSY and failed initialization remains retryable.

## Pinned SDK evidence

Audited SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu.a` members `emi.c.o` and `device_api.c.o`, decoded to LLVM IR in the Linux VM under `/tmp/jieli-pal-review-sdk/input/`. EMI copies writes of at most four bytes into its own static buffer; larger writes borrow the caller's buffer. Thus the one-byte stack command storage itself is safe. However, LCD RS must remain unchanged until completion, and the PAL now flushes before changing it.

`emi_wait_send_ok` logs a semaphore timeout, clears busy, and returns void. The FLUSH ioctl consequently returns zero even on that timeout. The ISR posts the semaphore before invoking the registered completion callback. The PAL now independently observes that callback, using the existing two-second completion budget. Failed/unproven completion retains the handle and DMA storage for a later close attempt. A flush with no pending transfer is skipped because the SDK would otherwise wait on an empty semaphore.

`dev_close` decrements the device reference before invoking the driver's close callback and does not restore it on error. Display and touch therefore report close errors but clear the consumed handle. `apps/common/iic/iic.c` takes its mutex before forwarding START and releases it after forwarding STOP even if the underlying ioctl fails. The PAL always attempts STOP after START and propagates both failures.

`cpu/wl82/key/adc_api.c` returns a channel index from `adc_add_sample_ch`, including valid positive indices; `ADC_MAX_CH` means full. The PAL checks that bound and GPIO errors. `adc_get_value` exposes no distinct error result: zero can be a valid sample, so it is not reinterpreted as failure.

## Host and native checks

`//tools/bazel:jieli_input_lifecycle_test` extracts the real provider and runs sixteen fault/interleaving scenarios. All sixteen fail against `17c7cd3f` and pass after the change under strict Clang and Linux VM GCC (`-Wall -Wextra -Werror`). Clang ThreadSanitizer passes, including real pthread draw/close, touch/close and ADC first-use contention. Cases cover failed and falsely successful flush, late completion, failed-open retention, close ownership, RS ordering, IIC START/STOP errors, ADC capacity/GPIO errors and concurrent operations.

Logs: `/tmp/jieli-input-lifecycle-expanded-before.log`, `/tmp/jieli-input-lifecycle-expanded-gcc-before.log`, `/tmp/jieli-input-lifecycle-final-after.log`, `/tmp/jieli-input-lifecycle-final-gcc.log`. Native Loader, PAL and Display packages build successfully in 70.603 seconds (`/tmp/jieli-input-lifecycle-native.log`). The iOS incompatible-target analysis passes (`/tmp/jieli-input-lifecycle-ios.log`).

The [hardware record](./pal-input-hardware.json) records a successful UART Display App install, matching App status and return to the unchanged Loader. Raw evidence is under `tmp/jieli/pal-review-next/diagnostic-runs/o6-display/`: `send.log`, `app.log`, `app.status`, and `returned.status`. The captured startup includes EMI open success, but does not contain the later display diagnostic report. This is transport/boot evidence only; SDK fault injection, physical touch/button actuation, teardown and electrical LCD correctness remain host-only or unverified on hardware.
