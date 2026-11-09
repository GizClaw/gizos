#include "h2_pcm_channels.h"

#include <assert.h>
#include <string.h>

static h2_audio_frame_t frame(void* data, size_t capacity, uint8_t channels) {
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = 16000u,
      .channels = channels,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  return h2_audio_frame_for_buffer(data, capacity, format);
}

static void test_extract_and_truncate(void) {
  /* Deliberately unaligned buffers; lanes are [100, -32768], [-100, 32767]. */
  uint8_t input[] = {0xff, 100, 0, 0, 128, 156, 255, 255, 127, 0xaa};
  uint8_t output[] = {0xaa, 0, 0, 0, 0, 0xaa};
  h2_audio_frame_t raw = frame(input + 1u, sizeof(input) - 1u, 2u);
  raw.bytes = 9u; /* An incomplete trailing frame is ignored. */
  h2_audio_frame_t out = frame(output + 1u, 4u, 1u);
  const uint8_t index = 1u;
  assert(h2_pcm_s16_select_mono(&raw, &out, &index, 1u) == H2_AUDIO_OK);
  const uint8_t expected[] = {0xaa, 0, 128, 255, 127, 0xaa};
  assert(memcmp(output, expected, sizeof(expected)) == 0);
  assert(out.bytes == 4u && out.samples_per_channel == 2u);

  out.capacity = 3u;
  assert(h2_pcm_s16_select_mono(&raw, &out, &index, 1u) == H2_AUDIO_OK);
  assert(out.bytes == 2u && out.samples_per_channel == 1u);
}

static void test_average_and_in_place(void) {
  /* Select lanes 1 and 2, excluding reference lane 0. Verify negative division
   * toward zero and full-scale arithmetic without narrowing before summation.
   */
  uint8_t input[] = {
      1, 0,   0,  128, 255, 127, 99,  0,   2,   0,   0,  128,
      0, 128, 98, 0,   3,   0,   255, 127, 255, 127, 97, 0,
  };
  h2_audio_frame_t raw = frame(input, sizeof(input), 4u);
  raw.bytes = sizeof(input);
  h2_audio_frame_t out = frame(input, sizeof(input), 1u);
  const uint8_t indices[] = {1u, 2u};
  assert(h2_pcm_s16_select_mono(&raw, &out, indices, 2u) == H2_AUDIO_OK);
  const uint8_t expected[] = {0, 0, 0, 128, 255, 127};
  assert(memcmp(input, expected, sizeof(expected)) == 0);
  assert(out.bytes == sizeof(expected) && out.samples_per_channel == 3u);

  uint8_t repeated[UINT8_MAX];
  memset(repeated, 0, sizeof(repeated));
  raw = frame(input + 4u, 2u, 1u);
  raw.bytes = 2u;
  out = frame(input, 2u, 1u);
  assert(h2_pcm_s16_select_mono(&raw, &out, repeated, UINT8_MAX) ==
         H2_AUDIO_OK);
  assert(input[0] == 255u && input[1] == 127u);
}

static void test_invalid_frames_are_atomic(void) {
  uint8_t input[] = {1, 0, 2, 0};
  uint8_t output[] = {0xa5, 0x5a};
  h2_audio_frame_t raw = frame(input, sizeof(input), 2u);
  raw.bytes = sizeof(input);
  h2_audio_frame_t out = frame(output, sizeof(output), 1u);
  out.bytes = 77u;
  out.samples_per_channel = 55u;
  const uint8_t index = 1u;

  assert(h2_pcm_s16_select_mono(NULL, &out, &index, 1u) ==
         H2_AUDIO_ERR_INVALID_ARG);
  assert(h2_pcm_s16_select_mono(&raw, NULL, &index, 1u) ==
         H2_AUDIO_ERR_INVALID_ARG);
  assert(h2_pcm_s16_select_mono(&raw, &out, NULL, 1u) ==
         H2_AUDIO_ERR_INVALID_ARG);
  assert(h2_pcm_s16_select_mono(&raw, &out, &index, 0u) ==
         H2_AUDIO_ERR_UNSUPPORTED);
  const uint8_t invalid_index = 2u;
  assert(h2_pcm_s16_select_mono(&raw, &out, &invalid_index, 1u) ==
         H2_AUDIO_ERR_UNSUPPORTED);

  for (unsigned int invalid = 0u; invalid < 10u; ++invalid) {
    h2_audio_frame_t bad_raw = raw;
    h2_audio_frame_t bad_out = out;
    int expected = H2_AUDIO_ERR_UNSUPPORTED;
    switch (invalid) {
      case 0u:
        bad_raw.data = NULL;
        break;
      case 1u:
        bad_out.data = NULL;
        break;
      case 2u:
        bad_raw.channels = 0u;
        break;
      case 3u:
        bad_out.channels = 2u;
        break;
      case 4u:
        bad_raw.sample_format = (h2_audio_sample_format_t)0;
        break;
      case 5u:
        bad_out.sample_rate_hz = 8000u;
        break;
      case 6u:
        bad_raw.bytes = 3u;
        expected = H2_AUDIO_ERR_INVALID_ARG;
        break;
      case 7u:
        bad_out.capacity = 1u;
        expected = H2_AUDIO_ERR_INVALID_ARG;
        break;
      case 8u:
        bad_raw.bytes = SIZE_MAX;
        expected = H2_AUDIO_ERR_INVALID_ARG;
        break;
      default:
        /* Reject unrepresentable sample metadata before accessing storage. */
        bad_raw.capacity = bad_raw.bytes = ((size_t)UINT16_MAX + 1u) * 4u;
        bad_out.capacity = ((size_t)UINT16_MAX + 1u) * 2u;
        expected = H2_AUDIO_ERR_INVALID_ARG;
        break;
    }
    assert(h2_pcm_s16_select_mono(&bad_raw, &bad_out, &index, 1u) == expected);
    assert(bad_out.bytes == 77u && bad_out.samples_per_channel == 55u);
    assert(output[0] == 0xa5u && output[1] == 0x5au);
  }
  raw.bytes = 0u;
  assert(h2_pcm_s16_select_mono(&raw, &out, &index, 1u) ==
         H2_AUDIO_ERR_INVALID_ARG);
  assert(out.bytes == 77u && out.samples_per_channel == 55u);
}

int main(void) {
  test_extract_and_truncate();
  test_average_and_in_place();
  test_invalid_frames_are_atomic();
  return 0;
}
