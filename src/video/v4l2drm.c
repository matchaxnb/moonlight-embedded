/*
 * V4L2 + DRM/KMS backend for Raspberry Pi 5.
 *
 * Path priority (best to worst):
 *
 * 1. Atomic DRM direct (zero-copy, no GPU, no dumb buffers):
 *      hevc_v4l2request → AVFrame DRM_PRIME NV12/SAND128
 *      → drmModeAddFB2WithModifiers → drmModeAtomicCommit
 *      Requires DRM_CAP_ADDFB2_MODIFIERS + DRM_CLIENT_CAP_ATOMIC.
 *      Skips EGL init and dumb buffer alloc entirely.
 *
 * 2. Legacy DRM direct (zero-copy, no GPU):
 *      Same as above but uses drmModeSetCrtc (first frame) +
 *      drmModePageFlip (subsequent) when atomic is unavailable.
 *
 * 3. EGL path (HAVE_EGL, GPU readback to dumb buffer):
 *      hevc_v4l2request → AVFrame DRM_PRIME SAND128
 *      → eglCreateImageKHR → GL_TEXTURE_EXTERNAL_OES
 *      → GLES2 passthrough shader → glReadPixels → dumb buf XRGB8888
 *      → drmModePageFlip
 *
 * 4. Swscale path (last resort, CPU copy):
 *      AVFrame DRM_PRIME → av_hwframe_transfer_data → swscale BGRA
 *      → dumb buf XRGB8888 → drmModePageFlip
 *
 * Moonlight is free software; GPL-3.0+
 */

#include "video.h"
#include "ffmpeg.h"
#include "../loop.h"
#include "../util.h"

#include <Limelight.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libswscale/swscale.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#ifdef HAVE_EGL
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT              0x3270
#define EGL_LINUX_DRM_FOURCC_EXT           0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT          0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT      0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT       0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT          0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT      0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT       0x3277
#endif
/* Required for rpivid SAND128 tiled frames (Pi 5 HEVC decoder) */
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#endif

typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC_LOCAL)(
    EGLenum platform, void* native_display, const EGLint* attrib_list);

#endif /* HAVE_EGL */

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <errno.h>

#define SLICES_PER_FRAME  4
#define NUM_DUMB_BUFS     2
#define FLIP_TIMEOUT_MS   20   /* max vblank wait; one frame at 60 Hz ≈ 16.7 ms */

struct dumb_buf {
  uint32_t handle, fb_id, stride, size;
  void*    map;
  int      w, h;
};

/* DRM state */
static int             drm_fd       = -1;
static int             render_fd    = -1;
static uint32_t        crtc_id      = 0;
static uint32_t        connector_id = 0;
static drmModeModeInfo drm_mode;
static drmModeCrtcPtr  saved_crtc   = NULL;
static volatile bool   flip_pending = false;

/* Swscale / dumb buffer state */
static struct dumb_buf    dumb_bufs[NUM_DUMB_BUFS];
static int                active_dumb  = 0;
static struct SwsContext* sws_ctx      = NULL;
static int                sws_src_fmt  = -1;
static int                sws_src_w    = 0;
static int                sws_src_h    = 0;
static int                sws_dst_w    = 0;
static int                sws_dst_h    = 0;
static int                sws_src_cs   = -1; /* AVColorSpace */
static int                sws_src_rng  = -1; /* AVColorRange */

/* EGL state */
#ifdef HAVE_EGL
static struct gbm_device* gbm_dev          = NULL;
static EGLDisplay         egl_dpy          = EGL_NO_DISPLAY;
static EGLContext         egl_ctx          = EGL_NO_CONTEXT;
static GLuint             gl_prog          = 0;
static GLuint             gl_vbo           = 0;
static GLuint             gl_fbo           = 0;
static GLuint             gl_fbo_tex       = 0;
static GLuint             gl_yuv_tex       = 0;
static GLint              gl_tex_loc       = -1;
static GLint              gl_attr_pos      = -1;
static GLint              gl_attr_uv       = -1;
static uint8_t*           readback_buf     = NULL;
static size_t             readback_sz      = 0;
static bool               egl_has_mod_ext  = false;
static bool               egl_frame_logged = false;

static PFNEGLCREATEIMAGEKHRPROC            pfn_eglCreateImageKHR     = NULL;
static PFNEGLDESTROYIMAGEKHRPROC           pfn_eglDestroyImageKHR    = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTex2D = NULL;
#endif /* HAVE_EGL */

/* DRM direct path state */
struct drm_prime_fb {
  uint32_t fb_id;
  AVFrame* frame;
};
static struct drm_prime_fb* drm_on_screen     = NULL;
static bool                 use_drm_direct    = false;
static bool                 drm_direct_active = false; /* true after first NV12 commit */

/* Atomic modesetting: property IDs for the primary plane */
struct drm_plane_props {
  uint32_t plane_id;
  uint32_t prop_crtc_id,   prop_fb_id;
  uint32_t prop_crtc_x,    prop_crtc_y,    prop_crtc_w,    prop_crtc_h;
  uint32_t prop_src_x,     prop_src_y,     prop_src_w,     prop_src_h;
  uint32_t prop_color_enc, prop_color_rng; /* 0 if driver doesn't expose them */
};
static struct drm_plane_props primary_plane;
static bool                   use_atomic = false;

/* YCbCr color metadata passed to the display engine per frame */
#define DRM_YCBCR_BT601   0u
#define DRM_YCBCR_BT709   1u
#define DRM_YCBCR_BT2020  2u
#define DRM_YCBCR_LIMITED 0u
#define DRM_YCBCR_FULL    1u
static uint64_t drm_color_enc = DRM_YCBCR_BT709;
static uint64_t drm_color_rng = DRM_YCBCR_LIMITED;

/* Decode / render thread state */
static void*   decode_buf      = NULL;
static size_t  decode_buf_sz   = 0;
static int     pipefd[2]       = {-1, -1};
static bool    prime_probed    = false;
static bool    use_egl_path    = false;
static bool    dumb_bufs_ready = false;

static pthread_t       render_tid;
static bool            render_started = false;
static pthread_mutex_t render_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  render_cond    = PTHREAD_COND_INITIALIZER;
static AVFrame*        render_queue[2] = {NULL, NULL};
static int             render_q_head  = 0; /* producer index */
static int             render_q_tail  = 0; /* consumer index */
static int             render_q_count = 0;
static bool            render_stop    = false;

/* Forward declarations */
static void do_page_flip(uint32_t fb_id);
static void render_swscale_frame(AVFrame* frame);
static bool render_drm_direct(AVFrame* frame);
static bool find_primary_plane(void);
static bool atomic_flip(uint32_t fb_id, int src_w, int src_h, void* user_data);
#ifdef HAVE_EGL
static bool render_egl_frame(AVFrame* frame);
#endif

/* Shared helpers */

static void close_gem_handle(uint32_t handle) {
  struct drm_gem_close gc = { .handle = handle };
  drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

/* Compute an aspect-ratio-correct destination rectangle centred in dw×dh.
 * Adds letterbox (top/bottom) or pillarbox (left/right) bars as needed. */
static void compute_dest_rect(int sw, int sh, int dw, int dh,
                               int* ox, int* oy, int* ow, int* oh) {
  if (sw > 0 && sh > 0 && sw * dh > sh * dw) {
    *ow = dw; *oh = sh * dw / sw;  /* wider than display: letterbox */
  } else if (sh > 0) {
    *oh = dh; *ow = sw * dh / sh;  /* taller or same ratio: pillarbox */
  } else {
    *ow = dw; *oh = dh;
  }
  *ox = (dw - *ow) / 2;
  *oy = (dh - *oh) / 2;
}

/* Release a drm_prime_fb: remove the KMS framebuffer, drop the frame ref. */
static void release_prime_fb(struct drm_prime_fb* fb) {
  if (!fb) return;
  drmModeRmFB(drm_fd, fb->fb_id);
  if (fb->frame) av_frame_free(&fb->frame);
  free(fb);
}

/* EGL shaders */
#ifdef HAVE_EGL
static const char* VS_SRC =
  "attribute vec2 a_pos;\n"
  "attribute vec2 a_uv;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "  v_uv = a_uv;\n"
  "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
  "}\n";

static const char* FS_SRC =
  "#extension GL_OES_EGL_image_external : require\n"
  "precision mediump float;\n"
  "uniform samplerExternalOES u_tex;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "  vec4 c = texture2D(u_tex, v_uv);\n"
  /* Mesa V3D converts YCbCr→RGB in hardware for NV12/SAND128 OES textures.
   * glReadPixels(GL_RGBA) writes [R,G,B,A]; DRM_FORMAT_XRGB8888 wants [B,G,R,X]. */
  "  gl_FragColor = vec4(c.b, c.g, c.r, 1.0);\n"
  "}\n";

static const EGLint EGL_CFG_ATTRS[] = {
  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
  EGL_RED_SIZE,   8,
  EGL_GREEN_SIZE, 8,
  EGL_BLUE_SIZE,  8,
  EGL_ALPHA_SIZE, 8,
  EGL_NONE
};
static const EGLint EGL_CTX_ATTRS[] = {
  EGL_CONTEXT_CLIENT_VERSION, 2,
  EGL_NONE
};
/* UV(0,0)=top-left (DMA-BUF convention). Quad places it at OpenGL bottom,
 * so glReadPixels row 0 = top of video — no flip needed in readback. */
static const float EGL_QUAD[] = {
  -1,-1, 0,0,
   1,-1, 1,0,
  -1, 1, 0,1,
   1, 1, 1,1,
};

static GLuint compile_shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {0};
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: shader error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static bool egl_init(int width, int height) {
  gbm_dev = gbm_create_device(render_fd);
  if (!gbm_dev) { fprintf(stderr, "EGL: gbm_create_device failed\n"); return false; }

  PFNEGLGETPLATFORMDISPLAYEXTPROC_LOCAL pfn_getPlatformDisplay =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC_LOCAL)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (pfn_getPlatformDisplay) {
    egl_dpy = pfn_getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
    printf("EGL: using eglGetPlatformDisplayEXT(GBM)\n");
  } else {
    setenv("EGL_PLATFORM", "gbm", 1);
    egl_dpy = eglGetDisplay((EGLNativeDisplayType)gbm_dev);
    printf("EGL: using eglGetDisplay with EGL_PLATFORM=gbm\n");
  }
  if (egl_dpy == EGL_NO_DISPLAY) {
    fprintf(stderr, "EGL: failed to get GBM display (err 0x%x)\n", eglGetError());
    return false;
  }

  EGLint major, minor;
  if (!eglInitialize(egl_dpy, &major, &minor)) {
    fprintf(stderr, "EGL: eglInitialize failed\n"); return false;
  }
  printf("EGL: version %d.%d\n", major, minor);

  const char* exts     = eglQueryString(egl_dpy, EGL_EXTENSIONS);
  bool has_dma         = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
  bool has_img         = exts && strstr(exts, "EGL_KHR_image_base");
  bool has_surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context");
  egl_has_mod_ext      = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");
  printf("EGL: dma_buf=%d image_base=%d surfaceless=%d modifiers=%d\n",
         has_dma, has_img, has_surfaceless, egl_has_mod_ext);
  if (!has_dma || !has_img || !has_surfaceless) {
    fprintf(stderr, "EGL: missing required extensions\n"); return false;
  }

  pfn_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
      eglGetProcAddress("eglCreateImageKHR");
  pfn_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
      eglGetProcAddress("eglDestroyImageKHR");
  pfn_glEGLImageTargetTex2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
      eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if (!pfn_eglCreateImageKHR || !pfn_eglDestroyImageKHR || !pfn_glEGLImageTargetTex2D) {
    fprintf(stderr, "EGL: extension functions not found\n"); return false;
  }

  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg; EGLint ncfg = 0;
  if (!eglChooseConfig(egl_dpy, EGL_CFG_ATTRS, &cfg, 1, &ncfg) || ncfg < 1) {
    fprintf(stderr, "EGL: eglChooseConfig failed (0x%x)\n", eglGetError()); return false;
  }

  egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, EGL_CTX_ATTRS);
  if (egl_ctx == EGL_NO_CONTEXT) {
    fprintf(stderr, "EGL: eglCreateContext failed (0x%x)\n", eglGetError()); return false;
  }

  if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
    fprintf(stderr, "EGL: eglMakeCurrent surfaceless failed (0x%x)\n", eglGetError());
    return false;
  }

  glGenFramebuffers(1, &gl_fbo);
  glGenTextures(1, &gl_fbo_tex);
  glBindTexture(GL_TEXTURE_2D, gl_fbo_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, gl_fbo_tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "EGL: FBO incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  printf("EGL: FBO created (%dx%d)\n", width, height);

  GLuint vs = compile_shader(GL_VERTEX_SHADER,   VS_SRC);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
  if (!vs || !fs) return false;
  gl_prog = glCreateProgram();
  glAttachShader(gl_prog, vs);
  glAttachShader(gl_prog, fs);
  glLinkProgram(gl_prog);
  glDeleteShader(vs); glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(gl_prog, GL_LINK_STATUS, &ok);
  if (!ok) { fprintf(stderr, "EGL: program link failed\n"); return false; }

  /* Cache locations once — querying per frame is needlessly expensive */
  gl_tex_loc  = glGetUniformLocation(gl_prog, "u_tex");
  gl_attr_pos = glGetAttribLocation(gl_prog, "a_pos");
  gl_attr_uv  = glGetAttribLocation(gl_prog, "a_uv");
  glUseProgram(gl_prog);
  glUniform1i(gl_tex_loc, 0);

  glGenBuffers(1, &gl_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(EGL_QUAD), EGL_QUAD, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  glGenTextures(1, &gl_yuv_tex);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_yuv_tex);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

  glViewport(0, 0, width, height);
  printf("EGL: initialized (%dx%d)\n", width, height);
  return true;
}

static void egl_destroy(void) {
  if (gl_fbo_tex) { glDeleteTextures(1, &gl_fbo_tex);    gl_fbo_tex = 0; }
  if (gl_fbo)     { glDeleteFramebuffers(1, &gl_fbo);    gl_fbo     = 0; }
  if (gl_yuv_tex) { glDeleteTextures(1, &gl_yuv_tex);    gl_yuv_tex = 0; }
  if (gl_vbo)     { glDeleteBuffers(1, &gl_vbo);         gl_vbo     = 0; }
  if (gl_prog)    { glDeleteProgram(gl_prog);             gl_prog    = 0; }
  if (egl_dpy != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
  }
  if (gbm_dev) gbm_device_destroy(gbm_dev);
  free(readback_buf);
  egl_dpy = EGL_NO_DISPLAY; egl_ctx = EGL_NO_CONTEXT; gbm_dev = NULL;
  readback_buf = NULL; readback_sz = 0;
  gl_attr_pos = -1; gl_attr_uv = -1;
  egl_has_mod_ext = false;
}

static bool render_egl_frame(AVFrame* frame) {
  if (!dumb_bufs_ready) return false;
  AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)frame->data[0];
  if (!desc || desc->nb_objects < 1 || desc->nb_layers < 1) return false;

  AVDRMLayerDescriptor* layer = &desc->layers[0];
  struct dumb_buf* buf = &dumb_bufs[active_dumb];

  int dw = drm_mode.hdisplay, dh = drm_mode.vdisplay;
  int ox, oy, ow, oh;
  compute_dest_rect(frame->width, frame->height, dw, dh, &ox, &oy, &ow, &oh);

  glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
  glViewport(0, 0, dw, dh);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(ox, oy, ow, oh);

  /* Build EGL attrib list for dma-buf import (max ~27 entries; 40 for headroom) */
  EGLint attribs[40]; int na = 0;
#define A(k,v) do { attribs[na++]=(k); attribs[na++]=(v); } while(0)
  A(EGL_WIDTH,                frame->width);
  A(EGL_HEIGHT,               frame->height);
  A(EGL_LINUX_DRM_FOURCC_EXT, layer->format);
  A(EGL_DMA_BUF_PLANE0_FD_EXT,     desc->objects[layer->planes[0].object_index].fd);
  A(EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)layer->planes[0].offset);
  A(EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)layer->planes[0].pitch);
  if (egl_has_mod_ext) {
    uint64_t m = desc->objects[layer->planes[0].object_index].format_modifier;
    A(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(m & 0xFFFFFFFFu));
    A(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)((m >> 32) & 0xFFFFFFFFu));
  }
  if (layer->nb_planes >= 2) {
    A(EGL_DMA_BUF_PLANE1_FD_EXT,     desc->objects[layer->planes[1].object_index].fd);
    A(EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)layer->planes[1].offset);
    A(EGL_DMA_BUF_PLANE1_PITCH_EXT,  (EGLint)layer->planes[1].pitch);
    if (egl_has_mod_ext) {
      uint64_t m = desc->objects[layer->planes[1].object_index].format_modifier;
      A(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, (EGLint)(m & 0xFFFFFFFFu));
      A(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, (EGLint)((m >> 32) & 0xFFFFFFFFu));
    }
  }
  attribs[na] = EGL_NONE;
#undef A

  if (!egl_frame_logged) {
    egl_frame_logged = true;
    uint64_t mod = desc->objects[layer->planes[0].object_index].format_modifier;
    printf("EGL: frame fourcc=0x%08x modifier=0x%016llx nb_layers=%d nb_planes=%d\n",
           layer->format, (unsigned long long)mod, desc->nb_layers, layer->nb_planes);
    printf("EGL: stream=%dx%d → %dx%d+%d+%d (display %dx%d)\n",
           frame->width, frame->height, ow, oh, ox, oy, dw, dh);
  }

  EGLImageKHR img = pfn_eglCreateImageKHR(
      egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (img == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "EGL: eglCreateImageKHR failed (0x%x)\n", eglGetError());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_yuv_tex);
  pfn_glEGLImageTargetTex2D(GL_TEXTURE_EXTERNAL_OES, img);
  pfn_eglDestroyImageKHR(egl_dpy, img);

  glUseProgram(gl_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl_yuv_tex);
  glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  glEnableVertexAttribArray(gl_attr_pos);
  glEnableVertexAttribArray(gl_attr_uv);
  glVertexAttribPointer(gl_attr_pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
  glVertexAttribPointer(gl_attr_uv,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                        (void*)(2*sizeof(float)));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(gl_attr_pos);
  glDisableVertexAttribArray(gl_attr_uv);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

  /* Readback: glReadPixels writes dw*4 bytes/row (no GLES2 GL_PACK_ROW_LENGTH).
   * Use an intermediate buffer then stride-aware copy to the DRM dumb buffer. */
  size_t row_bytes = (size_t)dw * 4;
  size_t needed    = row_bytes * (size_t)dh;
  if (needed > readback_sz) {
    free(readback_buf);
    readback_buf = malloc(needed);
    if (!readback_buf) {
      readback_sz = 0;
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      return false;
    }
    readback_sz = needed;
  }
  glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, readback_buf);
  if (buf->stride == (uint32_t)row_bytes) {
    memcpy(buf->map, readback_buf, needed);
  } else {
    for (int y = 0; y < dh; y++)
      memcpy((uint8_t*)buf->map + (size_t)y * buf->stride,
             readback_buf        + (size_t)y * row_bytes, row_bytes);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  do_page_flip(buf->fb_id);
  active_dumb ^= 1;
  return true;
}
#endif /* HAVE_EGL */

/* DRM page flip */
static void page_flip_handler(int fd, unsigned int seq,
                               unsigned int tv_sec, unsigned int tv_usec,
                               void* user_data) {
  (void)fd; (void)seq; (void)tv_sec; (void)tv_usec;
  /* DRM direct path passes the previous on-screen fb as user_data so it can
   * be freed here, after the display has scanned out the new frame.
   * EGL/swscale paths pass NULL; release_prime_fb(NULL) is a no-op. */
  release_prime_fb((struct drm_prime_fb*)user_data);
  flip_pending = false;
}

static void wait_for_flip(void) {
  if (!flip_pending) return;
  drmEventContext evctx = {
    .version           = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = page_flip_handler,
  };
  struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };
  if (poll(&pfd, 1, FLIP_TIMEOUT_MS) > 0 && (pfd.revents & POLLIN))
    drmHandleEvent(drm_fd, &evctx);
}

static void do_page_flip(uint32_t fb_id) {
  wait_for_flip();
  if (drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) < 0) {
    static int err_count = 0;
    if (++err_count <= 3)
      fprintf(stderr, "V4L2DRM: drmModePageFlip failed (%s), using SetCrtc\n",
              strerror(errno));
    drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &drm_mode);
    return;
  }
  flip_pending = true;
}

/* DRM helpers */
static int open_drm_display(void) {
  const char* cards[] = { "/dev/dri/card0", "/dev/dri/card1",
                           "/dev/dri/card2", "/dev/dri/card3", NULL };
  for (int i = 0; cards[i]; i++) {
    int fd = open(cards[i], O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) { close(fd); continue; }
    bool ok = false;
    for (int c = 0; c < res->count_connectors && !ok; c++) {
      drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
      if (conn) {
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) ok = true;
        drmModeFreeConnector(conn);
      }
    }
    drmModeFreeResources(res);
    if (ok) { printf("V4L2DRM: display on %s\n", cards[i]); drm_fd = fd; return 0; }
    close(fd);
  }
  fprintf(stderr, "V4L2DRM: no display DRM device\n");
  return -1;
}

static bool find_connector_crtc(void) {
  drmModeRes* res = drmModeGetResources(drm_fd);
  if (!res) return false;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector* c = drmModeGetConnector(drm_fd, res->connectors[i]);
    if (!c) continue;
    if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
      drmModeFreeConnector(c); continue;
    }
    connector_id = c->connector_id;
    drm_mode     = c->modes[0];
    /* Prefer the mode flagged DRM_MODE_TYPE_PREFERRED (set by the EDID parser). */
    for (int m = 0; m < c->count_modes; m++)
      if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { drm_mode = c->modes[m]; break; }
    for (int e = 0; e < c->count_encoders && !crtc_id; e++) {
      drmModeEncoder* enc = drmModeGetEncoder(drm_fd, c->encoders[e]);
      if (!enc) continue;
      for (int j = 0; j < res->count_crtcs && !crtc_id; j++)
        if (enc->possible_crtcs & (1u << j)) crtc_id = res->crtcs[j];
      drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(c);
    if (crtc_id) break;
  }
  drmModeFreeResources(res);
  return connector_id && crtc_id;
}

static bool alloc_dumb(struct dumb_buf* b, int w, int h) {
  struct drm_mode_create_dumb cd = { .width=(uint32_t)w, .height=(uint32_t)h, .bpp=32 };
  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) return false;
  b->handle = cd.handle; b->stride = cd.pitch; b->size = cd.size; b->w = w; b->h = h;
  uint32_t handles[4] = {cd.handle}, pitches[4] = {cd.pitch}, offsets[4] = {0};
  if (drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_XRGB8888,
                    handles, pitches, offsets, &b->fb_id, 0) < 0) return false;
  struct drm_mode_map_dumb md = { .handle = cd.handle };
  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) return false;
  b->map = mmap(NULL, cd.size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
  if (b->map == MAP_FAILED) { b->map = NULL; return false; }
  memset(b->map, 0, cd.size);
  return true;
}

static void free_dumb(struct dumb_buf* b) {
  if (b->map)    { munmap(b->map, b->size); b->map = NULL; }
  if (b->fb_id)  { drmModeRmFB(drm_fd, b->fb_id); b->fb_id = 0; }
  if (b->handle) {
    struct drm_mode_destroy_dumb dd = { .handle = b->handle };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    b->handle = 0;
  }
}

/* Atomic modesetting helpers */
static bool find_primary_plane(void) {
  /* Discover the primary plane for our CRTC and cache its property IDs. */
  uint32_t crtc_idx = UINT32_MAX;
  drmModeRes* res = drmModeGetResources(drm_fd);
  if (!res) return false;
  for (int i = 0; i < res->count_crtcs; i++)
    if (res->crtcs[i] == crtc_id) { crtc_idx = (uint32_t)i; break; }
  drmModeFreeResources(res);
  if (crtc_idx == UINT32_MAX) return false;

  drmModePlaneResPtr pres = drmModeGetPlaneResources(drm_fd);
  if (!pres) return false;

  bool found = false;
  for (uint32_t i = 0; i < pres->count_planes && !found; i++) {
    drmModePlanePtr plane = drmModeGetPlane(drm_fd, pres->planes[i]);
    if (!plane) continue;
    if (!(plane->possible_crtcs & (1u << crtc_idx))) {
      drmModeFreePlane(plane); continue;
    }
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
        drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) { drmModeFreePlane(plane); continue; }

    bool is_primary = false;
    struct drm_plane_props pp = { .plane_id = plane->plane_id };
    for (uint32_t j = 0; j < props->count_props; j++) {
      drmModePropertyPtr p = drmModeGetProperty(drm_fd, props->props[j]);
      if (!p) continue;
      uint32_t id = p->prop_id;
      if      (!strcmp(p->name, "type") &&
               props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) is_primary = true;
      else if (!strcmp(p->name, "CRTC_ID"))        pp.prop_crtc_id  = id;
      else if (!strcmp(p->name, "FB_ID"))          pp.prop_fb_id    = id;
      else if (!strcmp(p->name, "CRTC_X"))         pp.prop_crtc_x   = id;
      else if (!strcmp(p->name, "CRTC_Y"))         pp.prop_crtc_y   = id;
      else if (!strcmp(p->name, "CRTC_W"))         pp.prop_crtc_w   = id;
      else if (!strcmp(p->name, "CRTC_H"))         pp.prop_crtc_h   = id;
      else if (!strcmp(p->name, "SRC_X"))          pp.prop_src_x    = id;
      else if (!strcmp(p->name, "SRC_Y"))          pp.prop_src_y    = id;
      else if (!strcmp(p->name, "SRC_W"))          pp.prop_src_w    = id;
      else if (!strcmp(p->name, "SRC_H"))          pp.prop_src_h    = id;
      else if (!strcmp(p->name, "COLOR_ENCODING")) pp.prop_color_enc = id;
      else if (!strcmp(p->name, "COLOR_RANGE"))    pp.prop_color_rng = id;
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);

    if (is_primary && pp.prop_fb_id) { primary_plane = pp; found = true; }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(pres);
  if (found)
    printf("V4L2DRM: primary plane %u (COLOR_ENCODING=%s COLOR_RANGE=%s)\n",
           primary_plane.plane_id,
           primary_plane.prop_color_enc ? "yes" : "no",
           primary_plane.prop_color_rng ? "yes" : "no");
  return found;
}

static bool atomic_flip(uint32_t fb_id, int src_w, int src_h, void* user_data) {
  drmModeAtomicReqPtr req = drmModeAtomicAlloc();
  if (!req) return false;

  int dw = drm_mode.hdisplay, dh = drm_mode.vdisplay;
  int ox, oy, ow, oh;
  compute_dest_rect(src_w, src_h, dw, dh, &ox, &oy, &ow, &oh);
#define AP(prop, val) \
  drmModeAtomicAddProperty(req, primary_plane.plane_id, primary_plane.prop_##prop, (val))
  AP(crtc_id, crtc_id);
  AP(fb_id,   fb_id);
  AP(crtc_x,  (uint64_t)ox);
  AP(crtc_y,  (uint64_t)oy);
  AP(crtc_w,  (uint64_t)ow);
  AP(crtc_h,  (uint64_t)oh);
  AP(src_x,   0);
  AP(src_y,   0);
  AP(src_w,   (uint64_t)src_w << 16);
  AP(src_h,   (uint64_t)src_h << 16);
#undef AP

  /* Color metadata: tells the HVS which YCbCr matrix and range to use when
   * converting NV12 for scan-out. Silently skipped if driver lacks the props. */
  if (primary_plane.prop_color_enc)
    drmModeAtomicAddProperty(req, primary_plane.plane_id,
                             primary_plane.prop_color_enc, drm_color_enc);
  if (primary_plane.prop_color_rng)
    drmModeAtomicAddProperty(req, primary_plane.plane_id,
                             primary_plane.prop_color_rng, drm_color_rng);

  /* ALLOW_MODESET for the first commit (format change XRGB8888 → NV12);
   * omit it for subsequent same-format flips to avoid unnecessary overhead. */
  uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
  if (!drm_direct_active) flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

  int ret = drmModeAtomicCommit(drm_fd, req, flags, user_data);
  drmModeAtomicFree(req);
  if (ret < 0) {
    fprintf(stderr, "V4L2DRM: drmModeAtomicCommit failed (%s)\n", strerror(-ret));
    return false;
  }
  if (!drm_direct_active) {
    drm_direct_active = true;
    printf("V4L2DRM: direct path active — NV12+SAND128 → display engine (atomic)\n");
  }
  return true;
}

/* DRM direct render (zero-copy: NV12/SAND128 → display engine) */

/* Map AVFrame color metadata to DRM plane property values.
 * Untagged streams default to BT.709/limited per ITU-R for HD, BT.601 for SD. */
static void update_color_metadata(const AVFrame* frame) {
  switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      drm_color_enc = DRM_YCBCR_BT601;  break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      drm_color_enc = DRM_YCBCR_BT2020; break;
    case AVCOL_SPC_UNSPECIFIED:
    default:
      drm_color_enc = (frame->height >= 720) ? DRM_YCBCR_BT709 : DRM_YCBCR_BT601;
      break;
  }
  drm_color_rng = (frame->color_range == AVCOL_RANGE_JPEG)
                  ? DRM_YCBCR_FULL : DRM_YCBCR_LIMITED;
}

static bool render_drm_direct(AVFrame* frame) {
  AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)frame->data[0];
  if (!desc || desc->nb_layers < 1) return false;

  AVDRMLayerDescriptor* layer = &desc->layers[0];
  int nb = layer->nb_planes;

  uint32_t handles[4]   = {0};
  uint32_t pitches[4]   = {0};
  uint32_t offsets[4]   = {0};
  uint64_t modifiers[4] = {0};

  for (int i = 0; i < nb; i++) {
    int obj = layer->planes[i].object_index;
    if (drmPrimeFDToHandle(drm_fd, desc->objects[obj].fd, &handles[i]) < 0) {
      for (int j = 0; j < i; j++) close_gem_handle(handles[j]);
      return false;
    }
    pitches[i]   = layer->planes[i].pitch;
    offsets[i]   = layer->planes[i].offset;
    modifiers[i] = desc->objects[obj].format_modifier;
  }

  uint32_t fb_id = 0;
  int rc = drmModeAddFB2WithModifiers(drm_fd, frame->width, frame->height,
                                       layer->format, handles, pitches, offsets,
                                       modifiers, &fb_id, DRM_MODE_FB_MODIFIERS);
  /* Release our GEM refs immediately; KMS holds its own ref via the FB object. */
  for (int i = 0; i < nb; i++) close_gem_handle(handles[i]);

  if (rc < 0) {
    fprintf(stderr, "V4L2DRM: drmModeAddFB2WithModifiers failed (%s), "
            "disabling direct path\n", strerror(-rc));
    use_drm_direct = false;
    return false;
  }

  update_color_metadata(frame);

  if (!drm_direct_active) {
    static const char* enc_names[] = { "BT.601", "BT.709", "BT.2020" };
    printf("V4L2DRM: direct fourcc=0x%08x modifier=0x%016llx color=%s %s\n"
           "V4L2DRM: stream=%dx%d display=%dx%d\n",
           layer->format, (unsigned long long)modifiers[0],
           enc_names[drm_color_enc < 3 ? drm_color_enc : 1],
           drm_color_rng ? "full" : "limited",
           frame->width, frame->height,
           drm_mode.hdisplay, drm_mode.vdisplay);
  }

  struct drm_prime_fb* ref = malloc(sizeof(*ref));
  if (!ref) { drmModeRmFB(drm_fd, fb_id); return false; }
  ref->fb_id = fb_id;
  ref->frame = av_frame_clone(frame);
  if (!ref->frame) { free(ref); drmModeRmFB(drm_fd, fb_id); return false; }

  wait_for_flip();
  struct drm_prime_fb* old = drm_on_screen;

  if (use_atomic) {
    if (!atomic_flip(fb_id, frame->width, frame->height, old)) {
      release_prime_fb(ref);
      use_drm_direct = false;
      return false;
    }
  } else {
    /* Legacy: SetCrtc once to switch format, PageFlip for steady-state. */
    if (!drm_direct_active) {
      if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0,
                         &connector_id, 1, &drm_mode) < 0) {
        fprintf(stderr, "V4L2DRM: drmModeSetCrtc NV12 failed (%s), "
                "disabling direct path\n", strerror(errno));
        release_prime_fb(ref);
        use_drm_direct = false;
        return false;
      }
      drm_direct_active = true;
      printf("V4L2DRM: direct path active — NV12+SAND128 → display engine\n");
      release_prime_fb(old); /* SetCrtc is synchronous; old is safe to free now. */
      drm_on_screen = ref;
      flip_pending  = false;
      return true;
    }
    if (drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, old) < 0) {
      /* Try SetCrtc as synchronous fallback. */
      if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0,
                         &connector_id, 1, &drm_mode) < 0) {
        fprintf(stderr, "V4L2DRM: both PageFlip and SetCrtc failed (%s), "
                "disabling direct path\n", strerror(errno));
        release_prime_fb(ref);
        use_drm_direct = false;
        return false;
      }
      release_prime_fb(old);
      drm_on_screen = ref;
      flip_pending  = false;
      return true;
    }
  }

  drm_on_screen = ref;
  flip_pending  = true;
  return true;
}

/* Swscale fallback */
static void render_swscale_frame(AVFrame* frame) {
  if (!dumb_bufs_ready) return;
  struct dumb_buf* buf = &dumb_bufs[active_dumb];
  AVFrame* sw = frame; AVFrame* tmp = NULL;
  if (frame->format == AV_PIX_FMT_DRM_PRIME) {
    tmp = av_frame_alloc();
    if (!tmp) return;
    if (av_hwframe_transfer_data(tmp, frame, 0) < 0) { av_frame_free(&tmp); return; }
    sw = tmp;
  }
  int ox, oy, ow, oh;
  compute_dest_rect(sw->width, sw->height, buf->w, buf->h, &ox, &oy, &ow, &oh);

  int cs  = sw->colorspace;
  int rng = sw->color_range;
  if (!sws_ctx || sw->format != sws_src_fmt
                || sw->width  != sws_src_w
                || sw->height != sws_src_h
                || ow         != sws_dst_w
                || oh         != sws_dst_h
                || cs         != sws_src_cs
                || rng        != sws_src_rng) {
    if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = NULL; }
    sws_ctx = sws_getContext(sw->width, sw->height, (enum AVPixelFormat)sw->format,
                              ow, oh, AV_PIX_FMT_BGRA,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
    sws_src_fmt = sw->format; sws_src_w = sw->width; sws_src_h = sw->height;
    sws_dst_w = ow; sws_dst_h = oh; sws_src_cs = cs; sws_src_rng = rng;
    if (!sws_ctx) {
      fprintf(stderr, "V4L2DRM: sws_getContext failed\n");
      if (tmp) av_frame_free(&tmp);
      return;
    }

    /* Select the correct YCbCr→RGB matrix.
     * Default: BT.709 for HD (≥720), BT.601 for SD — matches update_color_metadata(). */
    int sws_cs;
    switch (cs) {
      case AVCOL_SPC_BT470BG:
      case AVCOL_SPC_SMPTE170M: sws_cs = SWS_CS_ITU601;  break;
      case AVCOL_SPC_BT2020_NCL:
      case AVCOL_SPC_BT2020_CL:  sws_cs = SWS_CS_BT2020;  break;
      default:
        sws_cs = (sw->height >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
        break;
    }
    int src_range = (rng == AVCOL_RANGE_JPEG) ? 1 : 0;
    sws_setColorspaceDetails(sws_ctx,
      sws_getCoefficients(sws_cs), src_range,
      sws_getCoefficients(SWS_CS_DEFAULT), 1, /* full-range RGB out */
      0, 1 << 16, 1 << 16);

    printf("V4L2DRM: swscale fallback %dx%d → %dx%d+%d+%d fmt=%d cs=%d%s\n",
           sw->width, sw->height, ow, oh, ox, oy, sw->format, sws_cs,
           src_range ? " full-range" : "");
  }
  uint8_t* dst[4]    = { (uint8_t*)buf->map + (size_t)oy * buf->stride + (size_t)ox * 4 };
  int      dst_ls[4] = { (int)buf->stride };
  sws_scale(sws_ctx, (const uint8_t* const*)sw->data, sw->linesize,
            0, sw->height, dst, dst_ls);
  do_page_flip(buf->fb_id);
  active_dumb ^= 1;
  if (tmp) av_frame_free(&tmp);
}

/* Render thread */

/* Try each render path in priority order; the first successful one wins. */
static void dispatch_frame(AVFrame* frame) {
  if (use_drm_direct && frame->format == AV_PIX_FMT_DRM_PRIME)
    if (render_drm_direct(frame)) return;
#ifdef HAVE_EGL
  if (use_egl_path && frame->format == AV_PIX_FMT_DRM_PRIME)
    if (render_egl_frame(frame)) return;
#endif
  render_swscale_frame(frame);
}

static void* render_thread_fn(void* arg) {
  (void)arg;
#ifdef HAVE_EGL
  if (use_egl_path) {
    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
      fprintf(stderr, "EGL: render thread eglMakeCurrent failed (0x%x)\n", eglGetError());
      use_egl_path = false;
    } else {
      printf("EGL: context bound to render thread\n");
    }
  }
#endif
  while (!render_stop) {
    pthread_mutex_lock(&render_mutex);
    while (!render_stop && render_q_count == 0)
      pthread_cond_wait(&render_cond, &render_mutex);
    AVFrame* frame = NULL;
    if (render_q_count > 0) {
      frame = render_queue[render_q_tail];
      render_queue[render_q_tail] = NULL;
      render_q_tail = (render_q_tail + 1) & 1;
      render_q_count--;
    }
    pthread_mutex_unlock(&render_mutex);
    if (!frame) continue;
    dispatch_frame(frame);
    av_frame_free(&frame);
  }
  return NULL;
}

/* Frame reception */
static int frame_handle(int fd) {
  /* ffmpeg_get_frame() returns dec_frames[current_frame], a pointer into the
   * decoder's internal frame pool — not an independently owned allocation.
   * We must av_frame_clone() to get a ref-counted copy before the decoder
   * recycles the pool slot. Do not av_frame_free() the pool frames. */
  AVFrame* frame = NULL;
  while (read(fd, &frame, sizeof(void*)) > 0) {}
  if (!frame) return LOOP_OK;
  if (!prime_probed) {
    prime_probed = true;
    const char* path =
      (frame->format == AV_PIX_FMT_DRM_PRIME && use_drm_direct) ? "DRM-direct"     :
      (frame->format == AV_PIX_FMT_DRM_PRIME && use_egl_path)   ? "EGL"            :
      (frame->format == AV_PIX_FMT_DRM_PRIME)                   ? "swscale+hwxfer" :
                                                                   "software";
    printf("V4L2DRM: %s path (fmt=%d)\n", path, frame->format);
  }
  AVFrame* clone = av_frame_clone(frame);
  if (!clone) return LOOP_OK;
  pthread_mutex_lock(&render_mutex);
  if (render_q_count == 2) {
    /* Queue full: drop oldest to keep latency minimal */
    av_frame_free(&render_queue[render_q_tail]);
    render_queue[render_q_tail] = NULL;
    render_q_tail = (render_q_tail + 1) & 1;
    render_q_count--;
  }
  render_queue[render_q_head] = clone;
  render_q_head = (render_q_head + 1) & 1;
  render_q_count++;
  pthread_cond_signal(&render_cond);
  pthread_mutex_unlock(&render_mutex);
  return LOOP_OK;
}

/* get_format callback */
static enum AVPixelFormat drm_prime_get_format(AVCodecContext* ctx,
                                               const enum AVPixelFormat* fmts) {
  (void)ctx;
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
  return fmts[0];
}

/* API callbacks */
static int v4l2drm_setup(int videoFormat, int width, int height,
                          int redrawRate, void* context, int drFlags) {
  (void)redrawRate; (void)context; (void)drFlags;
  ensure_buf_size(&decode_buf, &decode_buf_sz,
                  INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (open_drm_display() < 0) return -1;
  if (drmSetMaster(drm_fd) < 0)
    printf("V4L2DRM: drmSetMaster failed (non-root?)\n");
  if (!find_connector_crtc()) {
    fprintf(stderr, "V4L2DRM: no connected display\n"); return -2;
  }
  printf("V4L2DRM: %dx%d @ %u Hz (CRTC %u, connector %u)\n",
         drm_mode.hdisplay, drm_mode.vdisplay, drm_mode.vrefresh,
         crtc_id, connector_id);
  saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);

  /* Probe DRM direct path: requires display device to support FB modifiers. */
  use_drm_direct = false; use_atomic = false;
  uint64_t cap = 0;
  if (drmGetCap(drm_fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap) {
    use_drm_direct = true;
    /* Atomic modesetting: handles format changes natively (ALLOW_MODESET on
     * first commit). Preferred over legacy SetCrtc+PageFlip. */
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 &&
        drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
        find_primary_plane()) {
      use_atomic = true;
      printf("V4L2DRM: atomic modesetting enabled\n");
    } else {
      printf("V4L2DRM: DRM direct enabled (legacy flip path)\n");
    }
  } else {
    printf("V4L2DRM: DRM_CAP_ADDFB2_MODIFIERS not supported — will use EGL/swscale\n");
  }

  /* EGL: only needed when atomic DRM direct is not available. */
  use_egl_path = false;
#ifdef HAVE_EGL
  if (!use_atomic) {
    const char* rnodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", NULL };
    for (int i = 0; rnodes[i]; i++) {
      render_fd = open(rnodes[i], O_RDWR | O_CLOEXEC);
      if (render_fd >= 0) { printf("V4L2DRM: render node %s\n", rnodes[i]); break; }
    }
    if (render_fd >= 0 && egl_init(drm_mode.hdisplay, drm_mode.vdisplay)) {
      use_egl_path = true;
      printf("V4L2DRM: EGL path enabled\n");
      /* Release from main thread; render_thread_fn will rebind it. */
      eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else {
      printf("V4L2DRM: EGL unavailable, using swscale fallback\n");
      if (render_fd >= 0) { close(render_fd); render_fd = -1; }
    }
  }
#endif

  /* Always allocate dumb buffers and establish an initial XRGB8888 mode.
   * In atomic mode, the first NV12 commit (ALLOW_MODESET) switches the format.
   * If the NV12 path fails at runtime (plane doesn't support NV12+SAND128,
   * which can happen on CRT outputs like the Recalbox RGB Dual), swscale
   * takes over transparently using these buffers without a black screen. */
  dumb_bufs_ready = false;
  for (int i = 0; i < NUM_DUMB_BUFS; i++) {
    if (!alloc_dumb(&dumb_bufs[i], drm_mode.hdisplay, drm_mode.vdisplay)) {
      fprintf(stderr, "V4L2DRM: alloc_dumb[%d] failed\n", i); return -3;
    }
  }
  dumb_bufs_ready = true;
  if (drmModeSetCrtc(drm_fd, crtc_id, dumb_bufs[0].fb_id, 0, 0,
                     &connector_id, 1, &drm_mode) < 0) {
    perror("V4L2DRM: initial drmModeSetCrtc"); return -4;
  }

  int nproc   = (int)sysconf(_SC_NPROCESSORS_ONLN);
  int threads = (nproc >= 1 && nproc < SLICES_PER_FRAME) ? nproc : SLICES_PER_FRAME;
  printf("V4L2DRM: %d decode threads (%d CPUs)\n", threads, nproc);

  ffmpeg_get_format_cb = drm_prime_get_format;
  if (ffmpeg_init(videoFormat, width, height, SLICE_THREADING, 2, threads) < 0) {
    fprintf(stderr, "V4L2DRM: ffmpeg_init failed\n"); return -5;
  }

  if (pipe(pipefd) < 0) { perror("V4L2DRM: pipe"); return -6; }
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  loop_add_fd(pipefd[0], &frame_handle, POLLIN);

  render_stop    = false;
  render_started = false;
  if (pthread_create(&render_tid, NULL, render_thread_fn, NULL) != 0) {
    perror("V4L2DRM: pthread_create"); return -7;
  }
  render_started = true;
  return 0;
}

static void v4l2drm_cleanup(void) {
  if (render_started) {
    pthread_mutex_lock(&render_mutex);
    render_stop = true;
    for (int i = 0; i < 2; i++) {
      if (render_queue[i]) { av_frame_free(&render_queue[i]); render_queue[i] = NULL; }
    }
    render_q_head = render_q_tail = render_q_count = 0;
    pthread_cond_signal(&render_cond);
    pthread_mutex_unlock(&render_mutex);
    pthread_join(render_tid, NULL);
    render_started = false;
  }

  wait_for_flip();

  ffmpeg_destroy();

#ifdef HAVE_EGL
  if (use_egl_path) egl_destroy();
#endif
  if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = NULL; }
  if (pipefd[0] >= 0) { close(pipefd[0]); pipefd[0] = -1; }
  if (pipefd[1] >= 0) { close(pipefd[1]); pipefd[1] = -1; }

  /* Restore original CRTC before freeing DRM direct FBs: switching away from
   * NV12 first ensures the display never references a freed framebuffer. */
  if (saved_crtc && drm_fd >= 0) {
    drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                   saved_crtc->x, saved_crtc->y,
                   &connector_id, 1, &saved_crtc->mode);
    drmModeFreeCrtc(saved_crtc); saved_crtc = NULL;
  }

  /* Release the last on-screen NV12 FB (safe after CRTC restore above). */
  release_prime_fb(drm_on_screen); drm_on_screen = NULL;

  for (int i = 0; i < NUM_DUMB_BUFS; i++) free_dumb(&dumb_bufs[i]);
  if (drm_fd >= 0)    { drmDropMaster(drm_fd); close(drm_fd); drm_fd = -1; }
  if (render_fd >= 0) { close(render_fd); render_fd = -1; }

  free(decode_buf); decode_buf = NULL; decode_buf_sz = 0;
  prime_probed = false; use_egl_path = false; use_drm_direct = false;
  use_atomic = false; drm_direct_active = false; dumb_bufs_ready = false;
  flip_pending = false; active_dumb = 0; crtc_id = 0; connector_id = 0;
  drm_color_enc = DRM_YCBCR_BT709; drm_color_rng = DRM_YCBCR_LIMITED;
  sws_src_fmt = -1; sws_src_w = 0; sws_src_h = 0;
  sws_dst_w = 0; sws_dst_h = 0; sws_src_cs = -1; sws_src_rng = -1;
#ifdef HAVE_EGL
  egl_frame_logged = false;
#endif
  memset(&primary_plane, 0, sizeof(primary_plane));
}

static int v4l2drm_submit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList; int length = 0;
  ensure_buf_size(&decode_buf, &decode_buf_sz,
                  decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
  while (entry) {
    memcpy((uint8_t*)decode_buf + length, entry->data, entry->length);
    length += entry->length; entry = entry->next;
  }
  ffmpeg_decode(decode_buf, length);
  AVFrame* frame = ffmpeg_get_frame(true);
  if (frame) {
    ssize_t n = write(pipefd[1], &frame, sizeof(void*));
    (void)n;
  }
  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_v4l2drm = {
  .setup            = v4l2drm_setup,
  .cleanup          = v4l2drm_cleanup,
  .submitDecodeUnit = v4l2drm_submit,
  .capabilities     = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) |
                      CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC  |
                      CAPABILITY_DIRECT_SUBMIT,
};
