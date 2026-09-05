#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>

#define H2_WEB_MIC_SAMPLES 320u
#define H2_WEB_MIC_START_TIMEOUT_MS 30000u

// Keep browser callbacks in JS. Only the PAL caller touches its Wasm buffer;
// promises and worklet messages never reenter a running cooperative executor.
// clang-format off
EM_JS(int, h2_web_mic_supported_js, (), {
  return !!(globalThis.navigator?.mediaDevices?.getUserMedia &&
            globalThis.AudioContext && globalThis.AudioWorkletNode);
});

EM_JS(int, h2_web_mic_begin_js, (uintptr_t address), {
  const entries = Module['h2WebMicrophones'] ||= new Map();
  const streams = Module['h2WebMicrophoneStreams'] ||= new Map();
  if (entries.has(address)) return -7;
  const entry = {status: -9, queue: [], stream: null, context: null,
                 source: null, node: null, ended: null, url: null, released: false};
  entries.set(address, entry);
  const current = () => entries.get(address) === entry && !entry.released;
  entry.release = () => {
    if (entry.released) return;
    entry.released = true;
    entry.queue.length = 0;
    if (entry.url) { URL.revokeObjectURL(entry.url); entry.url = null; }
    if (streams.get(address) === entry.stream) streams.delete(address);
    if (entry.node) {
      entry.node.port.onmessage = null;
      entry.node.onprocessorerror = null;
      entry.node.port.close();
      entry.node.disconnect();
    }
    if (entry.source) entry.source.disconnect();
    if (entry.stream) {
      for (const track of entry.stream.getTracks()) {
        track.removeEventListener('ended', entry.ended);
        track.stop();
      }
    }
    if (entry.context) entry.context.close().catch(() => {});
  };
  const fail = (status) => {
    if (!current()) return;
    entry.status = status;
    entry.release();
  };
  // Acquire immediately, before the C caller yields away from a user gesture.
  try {
    // Resume synchronously while the caller still has transient user activation.
    const context = entry.context = new AudioContext({sampleRate: 16000});
    if (context.sampleRate !== 16000 || !context.audioWorklet) {
      fail(-3); return -3;
    }
    const resumed = context.resume().catch(() => { fail(-2); });
    const request = navigator.mediaDevices.getUserMedia({
      audio: {channelCount: 1, sampleRate: 16000, echoCancellation: true},
      video: false,
    });
    (async () => {
      const stream = await request;
      if (!current()) {
        stream.getTracks().forEach(track => track.stop());
        return;
      }
      entry.stream = stream;
      const tracks = stream.getAudioTracks();
      if (!tracks.length || tracks.some(track => track.readyState !== 'live')) {
        fail(-2); return;
      }
      const echo = tracks[0].getSettings().echoCancellation;
      console.info('Web microphone echoCancellation=' + (echo ?? 'unknown'));
      if (echo !== true && echo !== 'all' && echo !== 'remote-only')
        console.warn('Web microphone echo cancellation was not confirmed by the browser');
      entry.ended = () => fail(-10);
      tracks.forEach(track => track.addEventListener('ended', entry.ended));
      // Web Audio resamples the input device into this context's rate.
      function installMicrophone() {
        class H2Microphone extends AudioWorkletProcessor {
          constructor() {
            super();
            this.pool = [];
            this.frame = null;
            this.used = 0;
            this.port.onmessage = ({data}) => {
              if (data instanceof ArrayBuffer && data.byteLength === 640 &&
                  this.pool.length < 8) this.pool.push(new Int16Array(data));
            };
          }
          process(inputs) {
            const channels = inputs[0];
            if (!channels?.length) return true;
            for (let i = 0; i < channels[0].length; ++i) {
              if (!this.frame) this.frame = this.pool.pop() || null;
              if (!this.frame) continue; // bounded drop-newest under pressure
              let value = 0;
              for (const channel of channels) value += channel[i];
              value = Math.max(-1, Math.min(1, value / channels.length));
              this.frame[this.used++] = Math.round(value < 0 ? value * 32768 : value * 32767);
              if (this.used === 320) {
                this.port.postMessage(this.frame.buffer, [this.frame.buffer]);
                this.frame = null;
                this.used = 0;
              }
            }
            // Leave all outputs zero: capture must not monitor into speakers.
            return true;
          }
        }
        registerProcessor('h2-microphone', H2Microphone);
      }
      const url = entry.url = URL.createObjectURL(new Blob([
        '(' + installMicrophone.toString() + ')();'
      ], {type: 'text/javascript'}));
      try { await context.audioWorklet.addModule(url); }
      finally { URL.revokeObjectURL(url); entry.url = null; }
      if (!current()) return;
      const node = entry.node = new AudioWorkletNode(context, 'h2-microphone', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      node.port.onmessage = ({data}) => {
        if (!current()) return;
        if (!(data instanceof ArrayBuffer) || data.byteLength !== 640 ||
            entry.queue.length >= 8) { fail(-4); return; }
        entry.queue.push(data);
      };
      node.onprocessorerror = () => fail(-4);
      for (let i = 0; i < 8; ++i) {
        const buffer = new ArrayBuffer(640);
        node.port.postMessage(buffer, [buffer]);
      }
      entry.source = context.createMediaStreamSource(stream);
      entry.source.connect(node);
      node.connect(context.destination);
      await resumed;
      if (!current()) return;
      if (context.state !== 'running') { fail(-2); return; }
      streams.set(address, stream);
      entry.status = 0;
    })().catch(error => {
      if (!current()) return;
      console.error('Web microphone start failed', error);
      fail(error?.name === 'NotAllowedError' || error?.name === 'NotFoundError' ||
           error?.name === 'NotReadableError' ? -2 : -4);
    });
  } catch (error) {
    fail(-4);
  }
  return entry.status;
});

EM_JS(int, h2_web_mic_status_js, (uintptr_t address), {
  return Module['h2WebMicrophones']?.get(address)?.status ?? -10;
});

EM_JS(void, h2_web_mic_stop_js, (uintptr_t address), {
  const entries = Module['h2WebMicrophones'];
  const entry = entries?.get(address);
  if (!entry) return;
  entry.status = -10;
  entry.release();
  entries.delete(address);
});

EM_JS(int, h2_web_mic_read_js, (uintptr_t address, uint8_t *output), {
  const entry = Module['h2WebMicrophones']?.get(address);
  if (!entry) return -10;
  if (entry.status !== 0) return entry.status;
  const buffer = entry.queue.shift();
  if (!buffer) return -9;
  HEAPU8.set(new Uint8Array(buffer), output);
  entry.node.port.postMessage(buffer, [buffer]);
  return 0;
});
// clang-format on

static int h2_web_mic_pause(h2_web_platform_t *platform) {
  const int result =
      h2_pal_time_sleep_ms(h2_web_platform_time_api(platform), 1u);
  if (result == H2_PAL_ERR_INVALID_STATE) {
    // Root callers use Asyncify; task callers yield through libco instead.
    emscripten_sleep(1u);
    return H2_PAL_OK;
  }
  return result;
}

int h2_web_platform_mic_supported(void) { return h2_web_mic_supported_js(); }

int h2_web_platform_mic_stop(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  ++platform->mic_generation;
  h2_web_mic_stop_js((uintptr_t)platform);
  return H2_PAL_OK;
}

int h2_web_platform_mic_start(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (platform->shutting_down || platform->mic_starting)
    return H2_PAL_ERR_INVALID_STATE;
  if (!h2_web_mic_supported_js())
    return H2_PAL_ERR_UNSUPPORTED;
  platform->mic_starting = true;
  ++platform->mic_calls;
  const uint64_t generation = platform->mic_generation;
  const double deadline = emscripten_get_now() + H2_WEB_MIC_START_TIMEOUT_MS;
  int result = h2_web_mic_begin_js((uintptr_t)platform);
  while (result == H2_PAL_ERR_WOULD_BLOCK) {
    result = h2_web_mic_pause(platform);
    if (result != H2_PAL_OK)
      break;
    if (generation != platform->mic_generation) {
      result = H2_PAL_ERR_CLOSED;
      break;
    }
    result = h2_web_mic_status_js((uintptr_t)platform);
    if (result == H2_PAL_ERR_WOULD_BLOCK && emscripten_get_now() >= deadline) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
  }
  if (result != H2_PAL_OK && result != H2_PAL_ERR_INVALID_STATE &&
      generation == platform->mic_generation)
    h2_web_platform_mic_stop(platform);
  --platform->mic_calls;
  platform->mic_starting = false;
  return result;
}

int h2_web_platform_mic_read(void *user, h2_audio_frame_t *frame,
                             uint32_t timeout_ms) {
  h2_web_platform_t *platform = user;
  if (frame != NULL)
    frame->bytes = 0u;
  if (platform == NULL || frame == NULL || frame->data == NULL ||
      frame->capacity < H2_WEB_MIC_SAMPLES * sizeof(int16_t))
    return H2_PAL_ERR_INVALID_ARG;
  if (platform->mic_reading)
    return H2_PAL_ERR_BUSY;
  platform->mic_reading = true;
  ++platform->mic_calls;
  const uint64_t generation = platform->mic_generation;
  const double deadline = emscripten_get_now() + timeout_ms;
  int result;
  for (;;) {
    result = h2_web_mic_read_js((uintptr_t)platform, frame->data);
    if (result != H2_PAL_ERR_WOULD_BLOCK || timeout_ms == 0u)
      break;
    if (emscripten_get_now() >= deadline) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    result = h2_web_mic_pause(platform);
    if (result != H2_PAL_OK)
      break;
    if (generation != platform->mic_generation) {
      result = H2_PAL_ERR_CLOSED;
      break;
    }
  }
  if (result == H2_PAL_OK) {
    frame->bytes = H2_WEB_MIC_SAMPLES * sizeof(int16_t);
    frame->samples_per_channel = H2_WEB_MIC_SAMPLES;
    frame->sample_rate_hz = 16000u;
    frame->channels = 1u;
    frame->sample_format = H2_AUDIO_SAMPLE_S16LE;
  }
  --platform->mic_calls;
  platform->mic_reading = false;
  return result;
}
