/*
 * This file is part of Moonlight Embedded.
 *
 * Based on Moonlight Pc implementation
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

#include "ffmpeg.h"

#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include <Limelight.h>
#include <libavcodec/avcodec.h>

#ifdef HAVE_V4L2_DRM
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;

static int dec_frames_cnt;
static int current_frame, next_frame;

#ifdef HAVE_V4L2_DRM
static AVBufferRef* hw_device_ctx;

enum AVPixelFormat (*ffmpeg_get_format_cb)(AVCodecContext*,
    const enum AVPixelFormat*) = NULL;

static enum AVPixelFormat drm_hwaccel_get_format(AVCodecContext* ctx,
    const enum AVPixelFormat* fmts) {
  if (ffmpeg_get_format_cb)
    return ffmpeg_get_format_cb(ctx, fmts);

  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == AV_PIX_FMT_DRM_PRIME) return *p;

  return fmts[0];
}

static int try_init_drm_hwaccel(AVCodecContext* ctx) {
  const char* drm_devs[] = {
    "/dev/dri/renderD128", "/dev/dri/card0",
    "/dev/dri/card1", NULL
  };

  for (int i = 0; drm_devs[i]; i++) {
    if (access(drm_devs[i], F_OK) != 0) continue;

    int ret = av_hwdevice_ctx_create(&hw_device_ctx,
        AV_HWDEVICE_TYPE_DRM, drm_devs[i], NULL, 0);
    if (ret == 0) {
      ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
      ctx->get_format = drm_hwaccel_get_format;
      printf("FFmpeg: DRM hwaccel on %s\n", drm_devs[i]);
      return 0;
    }
  }

  int ret = av_hwdevice_ctx_create(&hw_device_ctx,
      AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0);
  if (ret == 0) {
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    ctx->get_format = drm_hwaccel_get_format;
    printf("FFmpeg: DRM hwaccel via auto-detection\n");
    return 0;
  }

  fprintf(stderr, "FFmpeg: DRM hwaccel unavailable\n");
  return -1;
}
#endif

enum decoders ffmpeg_decoder;

#define BYTES_PER_PIXEL 4

int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count) {
  const AVCodec* candidate;
  AVCodecContext* ctx;

  av_log_set_level(AV_LOG_WARNING);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
  avcodec_register_all();
#endif

  pkt = av_packet_alloc();
  if (pkt == NULL) {
    printf("Couldn't allocate packet\n");
    return -1;
  }

  ffmpeg_decoder = perf_lvl & VAAPI_ACCELERATION ? VAAPI : SOFTWARE;

  struct decoder_entry {
    const char* name;
    bool hwaccel;
  };

  decoder = NULL;
  decoder_ctx = NULL;

  const struct decoder_entry h264_decoders[] = {
    {"h264_v4l2m2m", false},
    {"h264_nvv4l2", false},
    {"h264_nvmpi", false},
    {"h264_omx", false},
    {"h264", false},
    {NULL, false},
  };

  const struct decoder_entry hevc_decoders[] = {
    {"hevc_v4l2m2m", false},
    {"hevc_nvv4l2", false},
    {"hevc_nvmpi", false},
    {"hevc_omx", false},
#ifdef HAVE_V4L2_DRM
    {"hevc_v4l2request", false},
    {"hevc", true},
#endif
    {"hevc", false},
    {NULL, false},
  };

  const struct decoder_entry av1_decoders[] = {
    {"libdav1d", false},
    {"av1", false},
    {NULL, false},
  };

  const struct decoder_entry* decoders = NULL;

  if (videoFormat & VIDEO_FORMAT_MASK_H264)
    decoders = h264_decoders;
  else if (videoFormat & VIDEO_FORMAT_MASK_H265)
    decoders = hevc_decoders;
  else if (videoFormat & VIDEO_FORMAT_MASK_AV1)
    decoders = av1_decoders;
  else {
    printf("Video format not supported\n");
    return -1;
  }

  for (const struct decoder_entry* e = decoders; e->name; e++) {
    if (ffmpeg_decoder == VAAPI && !e->hwaccel)
      continue;

    const AVCodec* candidate = avcodec_find_decoder_by_name(e->name);
    if (!candidate) continue;

    AVCodecContext* ctx = avcodec_alloc_context3(candidate);
    if (ctx == NULL) {
      printf("Couldn't allocate context\n");
      return -1;
    }

    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    ctx->err_recognition = AV_EF_EXPLODE;
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (perf_lvl & SLICE_THREADING) {
      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = thread_count;
    } else {
      ctx->thread_count = 1;
    }

#ifdef HAVE_VAAPI
    if (ffmpeg_decoder == VAAPI)
      vaapi_init(ctx);
#endif

#ifdef HAVE_V4L2_DRM
    if (e->hwaccel) {
      if (try_init_drm_hwaccel(ctx) < 0) {
        avcodec_free_context(&ctx);
        continue;
      }
    }
#endif

    int err = avcodec_open2(ctx, candidate, NULL);
    if (err < 0) {
      printf("Couldn't open codec: %s\n", candidate->name);
#ifdef HAVE_V4L2_DRM
      if (e->hwaccel && hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = NULL;
      }
#endif
      avcodec_free_context(&ctx);
      continue;
    }

    decoder = candidate;
    decoder_ctx = ctx;
  }

  if (decoder == NULL) {
    printf("Couldn't find decoder\n");
    return -1;
  }

  printf("Using FFmpeg decoder: %s\n", decoder->name);

  dec_frames_cnt = buffer_count;
  dec_frames = calloc(buffer_count, sizeof(AVFrame*));
  if (dec_frames == NULL) {
    fprintf(stderr, "Couldn't allocate frames");
    return -1;
  }

  for (int i = 0; i < buffer_count; i++) {
    dec_frames[i] = av_frame_alloc();
    if (dec_frames[i] == NULL) {
      fprintf(stderr, "Couldn't allocate frame");
      return -1;
    }
  }

  return 0;
}

void ffmpeg_destroy(void) {
  av_packet_free(&pkt);
  if (decoder_ctx) {
    avcodec_free_context(&decoder_ctx);
  }
#ifdef HAVE_V4L2_DRM
  if (hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx);
    hw_device_ctx = NULL;
  }
  ffmpeg_get_format_cb = NULL;
#endif
  if (dec_frames) {
    for (int i = 0; i < dec_frames_cnt; i++) {
      if (dec_frames[i])
        av_frame_free(&dec_frames[i]);
    }
    free(dec_frames);
    dec_frames = NULL;
  }
}

AVFrame* ffmpeg_get_frame(bool native_frame) {
  int err = avcodec_receive_frame(decoder_ctx, dec_frames[next_frame]);
  if (err == 0) {
    current_frame = next_frame;
    next_frame = (current_frame+1) % dec_frames_cnt;

    if (ffmpeg_decoder == SOFTWARE || native_frame)
      return dec_frames[current_frame];
  } else if (err != AVERROR(EAGAIN)) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Receive failed - %d/%s\n", err, errorstring);
  }
  return NULL;
}

int ffmpeg_decode(unsigned char* indata, int inlen) {
  int err;

  pkt->data = indata;
  pkt->size = inlen;

  err = avcodec_send_packet(decoder_ctx, pkt);
  if (err < 0) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Decode failed - %s\n", errorstring);
  }

  return err < 0 ? err : 0;
}
