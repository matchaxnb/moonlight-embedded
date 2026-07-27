/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <opus_multistream.h>
#include <alsa/asoundlib.h>

#ifndef WITH_ALSA_SELFCONFIGURATION
#define CHECK_RETURN(f) if ((rc = f) < 0) { printf("Alsa error code %d\n", rc); return -1; }
#endif

static snd_pcm_t *handle;
static OpusMSDecoder* decoder;
static short* pcmBuffer;
static int samplesPerFrame;

#ifdef WITH_ALSA_SELFCONFIGURATION

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  /* Remap FL-FR-C-LFE-RL-RR to FL-FR-RL-RR-C-LFE (ALSA order) */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }

  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * opusConfig->channelCount * samplesPerFrame);
  if (!pcmBuffer) return -1;

  int opus_err = 0;
  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount,
      opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &opus_err);
  if (!decoder) { printf("Alsa error: opus_multistream_decoder_create failed: %d\n", opus_err); return -1; }

  unsigned int pcmChannels = opusConfig->channelCount;
  unsigned int sampleRate = opusConfig->sampleRate;

  char* audio_device = (char*)context;
  if (!audio_device) audio_device = "default";

  static const char* const fallbacks[] = { "default", "hw:0,0", NULL };
  int rc_open = snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, 0);
  for (int i = 0; rc_open < 0 && fallbacks[i]; i++) {
    if (strcmp(audio_device, fallbacks[i]) == 0)
      continue;
    printf("Alsa: cannot open '%s' (%s), trying '%s'\n",
           audio_device, snd_strerror(rc_open), fallbacks[i]);
    audio_device = fallbacks[i];
    rc_open = snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, 0);
  }
  if (rc_open < 0) {
    printf("Alsa: cannot open any audio device: %s\n", snd_strerror(rc_open));
    return -1;
  }

  /*
   * snd_pcm_set_params configures format, access, channels, rate and latency
   * in a single call. It lets ALSA pick the optimal period for the hardware
   * (DMA I2S constraints, power-of-2, etc.) rather than forcing a fixed
   * period size that some I2S drivers may reject.
   * soft_resample=1 enables software resampling if the exact rate is not
   * supported (rare at 48000 Hz but defensive).
   * Target latency: 200 ms (absorbs network irregularities).
   */
  int rc = snd_pcm_set_params(handle,
                               SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               pcmChannels, sampleRate,
                               1,       /* soft_resample */
                               200000); /* latency us = 200 ms */
  if (rc < 0 && pcmChannels > 2) {
    /* Hardware (e.g. PCM5102A I2S DAC) is stereo-only: rebuild decoder for FL+FR. */
    printf("Alsa: %u channels not supported (%s), falling back to stereo\n",
           pcmChannels, snd_strerror(rc));
    pcmChannels = 2;
    rc = snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, sampleRate, 1, 200000);
    if (rc == 0) {
      opus_multistream_decoder_destroy(decoder);
      unsigned char stereo_map[2] = { 0, 1 };
      decoder = opus_multistream_decoder_create(sampleRate, 2,
          opusConfig->streams, opusConfig->coupledStreams,
          stereo_map, &opus_err);
      if (!decoder) {
        printf("Alsa: stereo decoder creation failed: %d\n", opus_err);
        snd_pcm_close(handle); handle = NULL; return -1;
      }
      free(pcmBuffer);
      pcmBuffer = malloc(sizeof(short) * 2 * samplesPerFrame);
      if (!pcmBuffer) { snd_pcm_close(handle); handle = NULL; return -1; }
      printf("Alsa: stereo fallback active (FL+FR from %u-ch stream)\n",
             opusConfig->channelCount);
    }
  }
  if (rc < 0) {
    printf("Alsa: snd_pcm_set_params failed on '%s': %s\n",
           audio_device, snd_strerror(rc));
    snd_pcm_close(handle);
    handle = NULL;
    return -1;
  }

  snd_pcm_uframes_t actual_buf = 0, actual_period = 0;
  snd_pcm_get_params(handle, &actual_buf, &actual_period);
  printf("Alsa: %s | %d Hz | %u ch | period=%lu frames | buffer=%lu frames (%.0f ms)\n",
         audio_device, sampleRate, pcmChannels,
         actual_period, actual_buf,
         (double)actual_buf * 1000.0 / sampleRate);

  return 0;
}

#else /* !WITH_ALSA_SELFCONFIGURATION */

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc;
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  /* The supplied mapping array has order: FL-FR-C-LFE-RL-RR-SL-SR
   * ALSA expects the order: FL-FR-RL-RR-C-LFE-SL-SR
   * We need copy the mapping locally and swap the channels around.
   */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }

  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * opusConfig->channelCount * samplesPerFrame);
  if (pcmBuffer == NULL)
    return -1;

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);
  if (!decoder) { printf("Alsa error: opus_multistream_decoder_create failed: %d\n", rc); return -1; }

  snd_pcm_hw_params_t *hw_params;
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_uframes_t period_size = (opusConfig->sampleRate * 20) / 1000; // 20 ms period
  snd_pcm_uframes_t buffer_size = 3 * period_size; // 60 ms buffer
  unsigned int sampleRate = opusConfig->sampleRate;

  char* audio_device = (char*) context;
  if (audio_device == NULL)
    audio_device = "default";

  /* Open PCM device for playback. */
  CHECK_RETURN(snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK))

  /* Set hardware parameters */
  CHECK_RETURN(snd_pcm_hw_params_malloc(&hw_params));
  CHECK_RETURN(snd_pcm_hw_params_any(handle, hw_params));
  CHECK_RETURN(snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
  CHECK_RETURN(snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE));
  CHECK_RETURN(snd_pcm_hw_params_set_rate_near(handle, hw_params, &sampleRate, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_channels(handle, hw_params, opusConfig->channelCount));
  CHECK_RETURN(snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size));
  CHECK_RETURN(snd_pcm_hw_params(handle, hw_params));
  snd_pcm_hw_params_free(hw_params);

  /* Set software parameters */
  CHECK_RETURN(snd_pcm_sw_params_malloc(&sw_params));
  CHECK_RETURN(snd_pcm_sw_params_current(handle, sw_params));
  CHECK_RETURN(snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params(handle, sw_params));
  snd_pcm_sw_params_free(sw_params);

  CHECK_RETURN(snd_pcm_prepare(handle));

  return 0;
}

#endif /* WITH_ALSA_SELFCONFIGURATION */

static void alsa_renderer_cleanup() {
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }

  if (handle != NULL) {
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    handle = NULL;
  }

  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    int rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    if (rc < 0) {
      rc = snd_pcm_recover(handle, rc, 0);
      if (rc == 0)
        rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    }

    if (rc<0)
      printf("Alsa error from writei: %d\n", rc);
    else if (decodeLen != rc)
      printf("Alsa short write, write %d frames\n", rc);
  } else if (decodeLen < 0) {
    printf("Opus error from decode: %d\n", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init = alsa_renderer_init,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
