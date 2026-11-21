/* The PAL borrows Module.h2WebAudioContext once it starts. Run only after
 * this tab acquires the profile lock and the runtime can receive state. */
function h2GardenInstallAudio(module) {
  const Audio = globalThis.AudioContext || globalThis.webkitAudioContext;
  if (!Audio) return;
  let context;
  let foreground = !document.hidden;
  const publish = () => module['_h2_garden_guard_web_audio_active'](
    foreground && context?.state === 'running' ? 1 : 0);
  const activate = event => {
    if (!event.isTrusted || !foreground) return;
    try {
      if (!context) {
        context = module['h2WebAudioContext'] ||= new Audio();
        context.addEventListener('statechange', publish);
      }
      if (context.state !== 'running') void context.resume().then(publish, publish);
      publish();
    } catch (_) { publish(); }
  };
  function visible(next) {
    foreground = next;
    publish();
    if (!context || context.state === 'closed') return;
    if (foreground) void context.resume().then(publish, publish);
    else void context.suspend().then(publish, publish);
  }
  // Capture runs before the PAL and cover actions. Pointer-up covers touch
  // activation and a trusted click also covers an accessible button action.
  for (const kind of ['pointerdown', 'pointerup', 'keydown', 'click'])
    window.addEventListener(kind, activate, {capture: true, passive: true});
  document.addEventListener('visibilitychange', () => visible(!document.hidden));
  window.addEventListener('pagehide', () => visible(false));
  window.addEventListener('pageshow', () => visible(!document.hidden));
}
