/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
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

#include "ffmpeg_vaapi.h"

#include <va/va.h>
#include <va/va_x11.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#define MAX_SURFACES 16

static AVBufferRef* device_ref;
static bool av1_support = false;

static void probe_av1_support(void) {
  av1_support = false;

#ifdef VAProfileAV1Profile0
  if (!device_ref) return;

  AVHWDeviceContext* device = (AVHWDeviceContext*)device_ref->data;
  AVVAAPIDeviceContext* va_ctx = device->hwctx;

  int max_profiles = vaMaxNumProfiles(va_ctx->display);
  if (max_profiles <= 0) return;

  VAProfile* profiles = malloc((size_t)max_profiles * sizeof(VAProfile));
  if (!profiles) return;

  int actual = 0;
  VAStatus st = vaQueryConfigProfiles(va_ctx->display, profiles, &actual);
  if (st == VA_STATUS_SUCCESS) {
    for (int i = 0; i < actual; i++) {
      if (profiles[i] == VAProfileAV1Profile0
#ifdef VAProfileAV1Profile1
          || profiles[i] == VAProfileAV1Profile1
#endif
      ) {
        av1_support = true;
        break;
      }
    }
  }
  free(profiles);

  if (av1_support)
    printf("VAAPI: AV1 hardware decoding available\n");
  else
    printf("VAAPI: AV1 hardware decoding not available on this GPU\n");
#else
  printf("VAAPI: AV1 detection skipped (libva < 2.11)\n");
#endif
}

bool vaapi_has_av1(void) {
  return av1_support;
}

static enum AVPixelFormat select_sw_format(AVCodecContext* context) {
  if (context->codec_id == AV_CODEC_ID_HEVC) {
    if (context->profile == FF_PROFILE_HEVC_MAIN_10 ||
        context->profile == FF_PROFILE_HEVC_REXT)
      return AV_PIX_FMT_P010;
  }
  if (context->sw_pix_fmt == AV_PIX_FMT_YUV420P10LE ||
      context->sw_pix_fmt == AV_PIX_FMT_YUV420P10BE ||
      context->sw_pix_fmt == AV_PIX_FMT_P010LE       ||
      context->sw_pix_fmt == AV_PIX_FMT_P010BE)
    return AV_PIX_FMT_P010;
  return AV_PIX_FMT_NV12;
}

static enum AVPixelFormat va_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
  if (hw_ctx == NULL) {
    fprintf(stderr, "Failed to initialize Vaapi buffer\n");
    return AV_PIX_FMT_NONE;
  }

  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  fr_ctx->format = AV_PIX_FMT_VAAPI;
  fr_ctx->sw_format = select_sw_format(context);
  if (fr_ctx->sw_format == AV_PIX_FMT_P010)
    printf("VAAPI: P010 surface format selected (10-bit content)\n");
  fr_ctx->width = context->coded_width;
  fr_ctx->height = context->coded_height;
  fr_ctx->initial_pool_size = MAX_SURFACES + 1;

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize VAAPI frame context");
    return AV_PIX_FMT_NONE;
  }

  context->pix_fmt = AV_PIX_FMT_VAAPI;
  context->hw_device_ctx = device_ref;
  context->hw_frames_ctx = hw_ctx;
  context->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
  return AV_PIX_FMT_VAAPI;
}

static int va_get_buffer(AVCodecContext* context, AVFrame* frame, int flags) {
  return av_hwframe_get_buffer(context->hw_frames_ctx, frame, 0);
}

int vaapi_init_lib(Display* display) {
  (void)display;

  const char* drm_nodes[] = {
    "/dev/dri/renderD128", "/dev/dri/renderD129",
    "/dev/dri/renderD130", "/dev/dri/renderD131", NULL
  };

  for (int i = 0; drm_nodes[i]; i++) {
    if (access(drm_nodes[i], R_OK) != 0) continue;
    if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI,
        drm_nodes[i], NULL, 0) == 0) {
      printf("VAAPI: initialized via %s\n", drm_nodes[i]);
      probe_av1_support();
      return 0;
    }
  }

  if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI,
      NULL, NULL, 0) == 0) {
    printf("VAAPI: initialized via auto-detection\n");
    probe_av1_support();
    return 0;
  }

  if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI,
      ":0", NULL, 0) == 0) {
    printf("VAAPI: initialized via display :0 (fallback)\n");
    probe_av1_support();
    return 0;
  }

  fprintf(stderr, "VAAPI: all initialization paths failed\n");
  return -1;
}

int vaapi_init(AVCodecContext* decoder_ctx) {
  decoder_ctx->get_format = va_get_format;
  decoder_ctx->get_buffer2 = va_get_buffer;
  return 0;
}

void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height) {
  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  vaPutSurface(va_ctx->display, surface, win,
      0, 0, dec_frame->width, dec_frame->height,
      0, 0, width, height, NULL, 0, 0);
}
