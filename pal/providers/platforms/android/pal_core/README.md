# Android PAL Core and owned View

The native provider implements Core, Display, Touch, audio, storage and network capabilities. `H2AndroidPlatform` adds an owned Android View around that provider: frame presentation, aspect-fit layout, pointer coordinate conversion and single-pointer tracking stay inside this package.

## Hosting a View

Load the JNI library that contains this provider before creating `com.gizclaw.gizos.pal.H2AndroidPlatform`. An AAR consumer loads `h2_pal_core`; a monolithic Bazel App loads its own library linked with `:view`. The SDK does not choose an application library name.

```java
H2AndroidPlatform platform = new H2AndroidPlatform(context, 480, 800);
container.addView(platform.getView());
long nativePlatform = platform.getNativeHandle();
```

Create, mount, cancel pointer input and close on the UI thread. The entry controls the parent container, layout, window policy and App lifecycle. In native entry code, borrow `h2_android_platform_display_api()` and `h2_android_platform_touch_api()` from the opaque handle and inject them into Runtime. The View implements presentation and native pointer updates; the host does not implement Canvas rendering or MotionEvent translation.

Pause the App worker and call `cancelPointer()` when the host becomes inactive. Stop and join Runtime and every other native borrower before calling `platform.close()`. Close removes the View, retires its input/bitmap and destroys the native provider; it is idempotent. A failed Runtime join must retain the owner and its View for cleanup retry.

The existing C `h2_android_platform_create(env, external_view, config)` interface remains available for hosts that provide their own View with `requestFrame()`. Pointer snapshot and frame-copy functions also remain compatible. All owners must be quiesced before native destruction.

## Build outputs and checks

- `:view` provides the Java owner and its always-linked JNI bridge to Bazel Android Apps.
- `:aar` packages `classes.jar`, native JNI exports, consumer callback retention rules, Prefab headers and the native library.
- `:touch_state_test` checks captured native pointer edges, complete taps and separate strokes between polls, bounded-queue recovery, coordinate validation and release behavior on a host compiler. Adjacent MOVE samples coalesce; DOWN/UP boundaries are buffered. If edges alone exhaust the queue, polling reports an I/O loss before restarting a held pointer with DOWN.

```sh
bazel test //pal/providers/platforms/android/pal_core:touch_state_test //tools/bazel/mobile:mobile_package_test
bazel build --config=android_arm64 //pal/providers/platforms/android/pal_core:aar
```
