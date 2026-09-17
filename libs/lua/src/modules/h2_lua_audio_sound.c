#include <math.h>
#include <string.h>

#include "../runtime/h2_lua_internal.h"

#define AUDIO_SOUND_METATABLE "h2.audio.sound"
#define AUDIO_SOUND_PI 3.14159265358979323846

typedef struct audio_sound_segment {
  size_t samples;
  double freq;
  double freq_end;
  double gain;
  int wave;
  int envelope;
} audio_sound_segment_t;

void h2_lua_audio_sound_unref(h2_lua_audio_sound_t *sound) {
  if (sound != NULL && --sound->references == 0u) {
    h2_lua_job_t *job = sound->job;
    job->audio_sound_bytes -= sound->bytes;
    h2_pal_mem_free(job->host->config.runtime->mem, sound);
  }
}

h2_lua_audio_sound_t *h2_lua_audio_check_sound(lua_State *state, int index) {
  h2_lua_audio_sound_t **handle =
      luaL_checkudata(state, index, AUDIO_SOUND_METATABLE);
  return *handle;
}

static int audio_sound_release(lua_State *state) {
  h2_lua_audio_sound_t **handle =
      luaL_checkudata(state, 1, AUDIO_SOUND_METATABLE);
  h2_lua_audio_sound_unref(*handle);
  *handle = NULL;
  return 0;
}

static int audio_sound_info(lua_State *state) {
  h2_lua_audio_sound_t *sound = h2_lua_audio_check_sound(state, 1);
  size_t bytes = sound == NULL ? 0u : sound->bytes;
  uint32_t rate = sound == NULL ? 0u : sound->format.sample_rate_hz;
  uint8_t channels = sound == NULL ? 0u : sound->format.channels;
  lua_createtable(state, 0, 4);
  lua_pushinteger(state, (lua_Integer)bytes);
  lua_setfield(state, -2, "bytes");
  lua_pushinteger(state, rate);
  lua_setfield(state, -2, "sample_rate");
  lua_pushinteger(state, channels);
  lua_setfield(state, -2, "channels");
  lua_pushnumber(
      state, rate == 0u ? 0 : (double)bytes * 1000.0 / (2.0 * channels * rate));
  lua_setfield(state, -2, "duration_ms");
  return 1;
}

static int audio_sound_error(lua_State *state, const char *error) {
  lua_pushnil(state);
  lua_pushstring(state, error);
  return 2;
}

static double audio_sound_number(lua_State *state, int table, const char *key,
                                 double fallback) {
  double value;
  lua_pushstring(state, key);
  lua_rawget(state, table);
  value = lua_isnil(state, -1)                 ? fallback
          : lua_type(state, -1) == LUA_TNUMBER ? lua_tonumber(state, -1)
                                               : NAN;
  lua_pop(state, 1);
  return value;
}

static int audio_sound_choice(lua_State *state, int table, const char *key,
                              const char *const *choices, int count) {
  int result = -1;
  lua_pushstring(state, key);
  lua_rawget(state, table);
  if (lua_isnil(state, -1)) {
    result = 0;
  } else if (lua_type(state, -1) == LUA_TSTRING) {
    size_t length;
    const char *value = lua_tolstring(state, -1, &length);
    for (int i = 0; i < count; ++i) {
      if (strlen(choices[i]) == length &&
          memcmp(value, choices[i], length) == 0) {
        result = i;
        break;
      }
    }
  }
  lua_pop(state, 1);
  return result;
}

int h2_lua_audio_new_sound(lua_State *state) {
  static const char *const waves[] = {"sine", "square", "triangle", "noise"};
  static const char *const envelopes[] = {"flat", "decay", "bell"};
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  audio_sound_segment_t segments[64];
  size_t count = 0u;
  size_t bytes = 0u;
  const char *pcm = NULL;
  double rate = 16000;
  double channels = 1;
  int table = 0;
  h2_lua_audio_sound_t **handle;
  h2_lua_audio_sound_t *sound;
  if (lua_type(state, 1) == LUA_TSTRING) {
    pcm = lua_tolstring(state, 1, &bytes);
    if (!lua_isnoneornil(state, 2)) {
      luaL_checktype(state, 2, LUA_TTABLE);
      table = 2;
    }
  } else {
    luaL_checktype(state, 1, LUA_TTABLE);
    table = 1;
    count = lua_rawlen(state, 1);
    if (count == 0u || count > 64u) {
      return audio_sound_error(state, "audio sound: invalid");
    }
  }
  if (table != 0) {
    rate = audio_sound_number(state, table, "sample_rate", 16000);
    channels = audio_sound_number(state, table, "channels", 1);
  }
  if (!isfinite(rate) || rate < 1 || rate > UINT32_MAX || floor(rate) != rate ||
      !isfinite(channels) || channels < 1 || channels > 2 ||
      floor(channels) != channels) {
    return audio_sound_error(state, "audio sound: invalid");
  }
  for (size_t i = 0u; i < count; ++i) {
    audio_sound_segment_t *segment = &segments[i];
    double ms;
    double samples;
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    table = lua_gettop(state);
    ms = audio_sound_number(state, table, "ms", NAN);
    segment->freq = audio_sound_number(state, table, "freq", 0);
    segment->freq_end =
        audio_sound_number(state, table, "freq_end", segment->freq);
    segment->gain = audio_sound_number(state, table, "gain", 0.2);
    segment->wave = audio_sound_choice(state, table, "wave", waves, 4);
    segment->envelope =
        audio_sound_choice(state, table, "envelope", envelopes, 3);
    lua_pop(state, 1);
    if (!isfinite(ms) || ms < 1 || ms > 10000 || floor(ms) != ms ||
        !isfinite(segment->freq) || segment->freq < 0 ||
        segment->freq > rate / 2 || !isfinite(segment->freq_end) ||
        segment->freq_end < 0 || segment->freq_end > rate / 2 ||
        !isfinite(segment->gain) || segment->gain < 0 || segment->gain > 1 ||
        segment->wave < 0 || segment->envelope < 0) {
      return audio_sound_error(state, "audio sound: invalid");
    }
    samples = floor(ms * rate / 1000);
    if (samples > (double)(SIZE_MAX / (size_t)(2 * channels))) {
      return audio_sound_error(state, "audio sound: limit reached");
    }
    segment->samples = (size_t)samples;
    size_t segment_bytes = segment->samples * (size_t)(2 * channels);
    if (segment_bytes > SIZE_MAX - bytes) {
      return audio_sound_error(state, "audio sound: limit reached");
    }
    bytes += segment_bytes;
  }
  if (pcm != NULL && (bytes == 0u || bytes % (size_t)(2 * channels) != 0u)) {
    return audio_sound_error(state, "audio sound: invalid");
  }
  /* Install a finalizable empty handle before allocating Runtime memory. */
  if (luaL_newmetatable(state, AUDIO_SOUND_METATABLE)) {
    lua_pushcfunction(state, audio_sound_release);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, audio_sound_release);
    lua_setfield(state, -2, "release");
    lua_pushcfunction(state, audio_sound_info);
    lua_setfield(state, -2, "info");
    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "__index");
  }
  lua_pop(state, 1);
  handle = lua_newuserdatauv(state, sizeof(*handle), 0);
  *handle = NULL;
  luaL_setmetatable(state, AUDIO_SOUND_METATABLE);
  if (bytes > job->host->config.audio_sound_bytes_per_job -
                  job->audio_sound_bytes ||
      bytes > SIZE_MAX - sizeof(*sound)) {
    return audio_sound_error(state, "audio sound: limit reached");
  }
  sound =
      h2_pal_mem_alloc(job->host->config.runtime->mem, sizeof(*sound) + bytes);
  if (sound == NULL) {
    return audio_sound_error(state, "audio sound: no memory");
  }
  sound->job = job;
  sound->references = 1u;
  sound->bytes = bytes;
  sound->format = (h2_audio_pcm_format_t){
      .sample_rate_hz = (uint32_t)rate,
      .channels = (uint8_t)channels,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  job->audio_sound_bytes += bytes;
  *handle = sound;
  if (pcm != NULL) {
    memcpy(sound->pcm, pcm, bytes);
  } else {
    double phase = 0;
    uint32_t noise = 1u;
    size_t offset = 0u;
    for (size_t i = 0u; i < count; ++i) {
      const audio_sound_segment_t *segment = &segments[i];
      for (size_t j = 0u; j < segment->samples; ++j) {
        double u = segment->samples <= 1u
                       ? 0
                       : (double)j / (double)(segment->samples - 1u);
        double freq = segment->freq + (segment->freq_end - segment->freq) * u;
        double value;
        double envelope = 1;
        noise = noise * 1664525u + 1013904223u;
        switch (segment->wave) {
          case 1:
            value = phase < 0.5 ? 1 : -1;
            break;
          case 2:
            value = 1 - 4 * fabs(phase - 0.5);
            break;
          case 3:
            value = (double)(noise >> 8) / 8388607.5 - 1;
            break;
          default:
            value = sin(2 * AUDIO_SOUND_PI * phase);
            break;
        }
        if (segment->envelope == 1) {
          envelope = (1 - u) * (1 - u);
        } else if (segment->envelope == 2) {
          envelope = sin(AUDIO_SOUND_PI * u);
          envelope *= envelope;
        }
        int16_t sample =
            freq == 0 ? 0 : (int16_t)(value * segment->gain * envelope * 32767);
        for (uint8_t channel = 0u; channel < sound->format.channels;
             ++channel) {
          sound->pcm[offset++] = (uint8_t)(uint16_t)sample;
          sound->pcm[offset++] = (uint8_t)((uint16_t)sample >> 8);
        }
        phase += freq / rate;
        phase -= floor(phase);
      }
    }
  }
  return 1;
}

void h2_lua_audio_sound_stop(h2_lua_audio_track_slot_t *slot) {
  h2_lua_audio_sound_unref(slot->sound);
  slot->sound = NULL;
  slot->sound_offset = 0u;
}

void h2_lua_audio_sound_pump(h2_lua_audio_track_slot_t *slot) {
  h2_lua_audio_sound_t *sound = slot->sound;
  if (sound == NULL || slot->track == NULL) {
    return;
  }
  size_t sample_bytes = h2_audio_pcm_frame_bytes(&slot->format);
  size_t chunk = sample_bytes * (slot->format.frame_samples_per_channel == 0u
                                     ? UINT16_MAX
                                     : slot->format.frame_samples_per_channel);
  while (slot->sound_offset < sound->bytes) {
    size_t remaining = sound->bytes - slot->sound_offset;
    size_t take = remaining < chunk ? remaining : chunk;
    size_t written = take;
    void *data = sound->pcm + slot->sound_offset;
    if (take < chunk && slot->format.frame_samples_per_channel != 0u) {
      if (slot->carry == NULL) {
        slot->carry =
            h2_pal_mem_alloc(slot->job->host->config.runtime->mem, chunk);
        if (slot->carry == NULL) {
          return;
        }
      }
      memcpy(slot->carry, data, take);
      memset(slot->carry + take, 0, chunk - take);
      data = slot->carry;
      written = chunk;
    }
    h2_audio_frame_t frame =
        h2_audio_frame_for_buffer(data, written, slot->format);
    frame.bytes = written;
    frame.samples_per_channel = (uint16_t)(written / sample_bytes);
    if (h2_pal_audio_track_write(slot->track, &frame, 0u) != H2_PAL_OK) {
      return;
    }
    slot->sound_offset += take;
  }
  h2_lua_audio_sound_stop(slot);
}
