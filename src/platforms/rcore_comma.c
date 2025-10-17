/**********************************************************************************************
*
*   rcore_<platform> template - Functions to manage window, graphics device and inputs
*
*   PLATFORM: <PLATFORM>
*       - TODO: Define the target platform for the core
*
*   LIMITATIONS:
*       - Limitation 01
*       - Limitation 02
*
*   POSSIBLE IMPROVEMENTS:
*       - Improvement 01
*       - Improvement 02
*
*   ADDITIONAL NOTES:
*       - TRACELOG() function is located in raylib [utils] module
*
*   CONFIGURATION:
*       #define RCORE_PLATFORM_CUSTOM_FLAG
*           Custom flag for rcore on target platform -not used-
*
*   DEPENDENCIES:
*       - <platform-specific SDK dependency>
*       - gestures: Gestures system for touch-ready devices (or simulated from mouse inputs)
*
*
*   LICENSE: zlib/libpng
*
*   Copyright (c) 2013-2024 Ramon Santamaria (@raysan5) and contributors
*
*   This software is provided "as-is", without any express or implied warranty. In no event
*   will the authors be held liable for any damages arising from the use of this software.
*
*   Permission is granted to anyone to use this software for any purpose, including commercial
*   applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*     1. The origin of this software must not be misrepresented; you must not claim that you
*     wrote the original software. If you use this software in a product, an acknowledgment
*     in the product documentation would be appreciated but is not required.
*
*     2. Altered source versions must be plainly marked as such, and must not be misrepresented
*     as being the original software.
*
*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <linux/input.h>

#include <EGL/egl.h>
#include <EGL/eglplatform.h>

#include <GLES2/gl2.h>

#include <gbm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>


//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

typedef enum {
    FINGER_STATE_REMOVED = 0, // state when finger was removed and we handled its removal + default state
    FINGER_STATE_REMOVING, // state when finger is currently being removed from panel (released event)
    FINGER_STATE_TOUCHING, // state when finger is touching panel at any time
} FingerState;

struct finger {
  FingerState state;
  int x;
  int y;
  bool resetNextFrame;
};

struct touch {
  struct finger fingers[MAX_TOUCH_POINTS];
  int fd;
};

// hold all the low level egl stuff
struct egl_platform {
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
};

// hold all the low level drm stuff
struct drm_platform {
  int fd;

  uint32_t connector_id;
  uint32_t crtc_id;

  drmModeModeInfo mode;
};

// hold all the low level gbm stuff
struct gbm_platform {
  struct gbm_device *device;
  struct gbm_surface *surface;
  struct gbm_bo *current_bo;
  struct gbm_bo *next_bo;
  uint32_t current_fb;
  uint32_t next_fb;
};

typedef struct {
    struct egl_platform egl;
    struct touch touch;
    struct drm_platform drm;
    struct gbm_platform gbm;
    bool canonical_zero;
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { 0 };   // Platform specific data

//----------------------------------------------------------------------------------
// comma specific code
//----------------------------------------------------------------------------------

#define CASE_STR( value ) case value: return #value;
const char *eglGetErrorString(EGLint error) {
    switch(error) {
      CASE_STR( EGL_SUCCESS             )
      CASE_STR( EGL_NOT_INITIALIZED     )
      CASE_STR( EGL_BAD_ACCESS          )
      CASE_STR( EGL_BAD_ALLOC           )
      CASE_STR( EGL_BAD_ATTRIBUTE       )
      CASE_STR( EGL_BAD_CONTEXT         )
      CASE_STR( EGL_BAD_CONFIG          )
      CASE_STR( EGL_BAD_CURRENT_SURFACE )
      CASE_STR( EGL_BAD_DISPLAY         )
      CASE_STR( EGL_BAD_SURFACE         )
      CASE_STR( EGL_BAD_MATCH           )
      CASE_STR( EGL_BAD_PARAMETER       )
      CASE_STR( EGL_BAD_NATIVE_PIXMAP   )
      CASE_STR( EGL_BAD_NATIVE_WINDOW   )
      CASE_STR( EGL_CONTEXT_LOST        )
      default: return "Unknown";
    }
}
#undef CASE_STR

// These are actually float16s, so need to be converted before use
struct __attribute__((__packed__)) color_correction_values {
  uint16_t gamma;
  uint16_t ccm[9];
  uint16_t rgb_color_gains[3];
};

static const char *color_correction_fragment_shader_template =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "void main() {\n"
    "    vec4 c = texture2D(texture0, fragTexCoord) * fragColor * colDiffuse;\n"
    "    c.rgb = pow(c.rgb, vec3(2.2, 2.2, 2.2));\n"
    "    c.r *= %f;\n"
    "    c.g *= %f;\n"
    "    c.b *= %f;\n"
    "    vec3 rgb_cc = vec3(0.0, 0.0, 0.0);\n"
    "    rgb_cc += c.r * vec3(%f, %f, %f);\n"
    "    rgb_cc += c.g * vec3(%f, %f, %f);\n"
    "    rgb_cc += c.b * vec3(%f, %f, %f);\n"
    "    c.rgb = rgb_cc;\n"
    "    c.rgb = pow(c.rgb, vec3(%f/2.2, %f/2.2, %f/2.2));\n"
    "    gl_FragColor = c;\n"
    "}\n";

float decode_float16(uint16_t value){
  uint32_t sign = value >> 15;
  uint32_t exponent = (value >> 10) & 0x1F;
  uint32_t fraction = (value & 0x3FF);
  uint32_t output;
  if (exponent == 0){
    if (fraction == 0){
      // Zero
      output = (sign << 31);
    } else {
      exponent = 127 - 14;
      while ((fraction & (1 << 10)) == 0) {
        exponent--;
        fraction <<= 1;
      }
      fraction &= 0x3FF;
      output = (sign << 31) | (exponent << 23) | (fraction << 13);
    }
  } else if (exponent == 0x1F) {
    // Inf or NaN
    output = (sign << 31) | (0xFF << 23) | (fraction << 13);
  } else {
    // Regular
    output = (sign << 31) | ((exponent + (127-15)) << 23) | (fraction << 13);
  }

  return *((float*)&output);
}

struct color_correction_values * read_correction_values(void) {
  int ret;
  FILE *f = NULL;
  struct color_correction_values *ccv = NULL;

  if (getenv("DISABLE_COLOR_CORRECTION")) {
    TRACELOG(LOG_WARNING, "COMMA: Color correction disabled by flag");
    goto err;
  }

  ccv = malloc(sizeof(struct color_correction_values));
  if (ccv == NULL) {
    TRACELOG(LOG_WARNING, "COMMA: CCV allocation failed...");
    goto err;
  }

  const char *cal_paths[] = {
    getenv("COLOR_CORRECTION_PATH"),
    "/data/misc/display/color_cal/color_cal",
    "/sys/devices/platform/soc/894000.i2c/i2c-2/2-0017/color_cal",
    "/persist/comma/color_cal",
  };
  for (int i = 0; i < sizeof(cal_paths) / sizeof(const char *); i++) {
    const char *cal_fn = cal_paths[i];
    if (cal_fn == NULL) {
      continue;
    }

    TRACELOG(LOG_INFO, "COMMA: Color calibration trying %s", cal_fn);
    f = fopen(cal_fn, "r");
    if (f == NULL) {
      TRACELOG(LOG_INFO, "COMMA: - unable to open %s", cal_fn);
      continue;
    }

    ret = fread(ccv, sizeof(struct color_correction_values), 1, f);
    fclose(f);
    if (ret == 1) {
      return ccv;
    } else {
      TRACELOG(LOG_INFO, "COMMA: - file too short");
    }
  }

  TRACELOG(LOG_INFO, "COMMA: No color calibraion files found");

err:
  if (f != NULL) fclose(f);
  if (ccv != NULL) free(ccv);
  return NULL;
}

static int init_color_correction(void) {
  int ret;
  char *shader = NULL;
  struct color_correction_values *ccv = NULL;

  shader = malloc(1024);
  if(shader == NULL){
    TRACELOG(LOG_WARNING, "COMMA: Failed to malloc color correction");
    goto err;
  }

  ccv = read_correction_values();
  if(ccv == NULL){
    TRACELOG(LOG_INFO, "COMMA: No color correction values found");
    goto err;
  }

  ret = sprintf(shader,
    color_correction_fragment_shader_template,
    (1.0/decode_float16(ccv->rgb_color_gains[0])),
    (1.0/decode_float16(ccv->rgb_color_gains[1])),
    (1.0/decode_float16(ccv->rgb_color_gains[2])),
    decode_float16(ccv->ccm[0]),
    decode_float16(ccv->ccm[1]),
    decode_float16(ccv->ccm[2]),
    decode_float16(ccv->ccm[3]),
    decode_float16(ccv->ccm[4]),
    decode_float16(ccv->ccm[5]),
    decode_float16(ccv->ccm[6]),
    decode_float16(ccv->ccm[7]),
    decode_float16(ccv->ccm[8]),
    (1.0/decode_float16(ccv->gamma)),
    (1.0/decode_float16(ccv->gamma)),
    (1.0/decode_float16(ccv->gamma))
  );

  if(ret < 0){
    TRACELOG(LOG_WARNING, "COMMA: Color correction sprintf failed");
    goto err;
  }

  TRACELOG(LOG_INFO, "COMMA: Successfully setup color correction");
  free(ccv);

  CORE.Window.color_correction_shader_src = shader;
  return 0;

err:
  if (ccv != NULL) free(ccv);
  if (shader != NULL) free(shader);
  return -1;
}

static int recv_fd(int sock) {
  struct msghdr msg = {0};
  char m = 0;
  struct iovec io = { .iov_base = &m, .iov_len = 1 };

  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  memset(cmsgbuf, 0, sizeof(cmsgbuf));

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  if (recvmsg(sock, &msg, 0) < 0) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to receive from magic");
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
    errno = EPROTO;
    return -1;
  }

  int fd = -1;
  memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  return fd;
}

static int init_drm (const char *dev_path) {
  const char *s = getenv("DRM_FD");
  if (s) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 0 || v > 0x7fffffff) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to get drm device from env");
      return -1;
    }
    platform.drm.fd = (int)v;
  } else if (getenv("NO_MASTER")) {
    platform.drm.fd = open(dev_path, O_RDONLY | O_NONBLOCK);
  } else {
    const char *sock_path = "/tmp/drmfd.sock";
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to open socket to magic");
      return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to connect to magic");
      return -1;
    }

    platform.drm.fd = recv_fd(s);
  }

  if (platform.drm.fd < 0) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open drm device at %s", dev_path);
    return -1;
  }

  if (!drmIsMaster(platform.drm.fd)) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to get master role on %s", dev_path);
    return -1;
  }

  drmModeConnector *connector = NULL;
  drmModeRes *res = drmModeGetResources(platform.drm.fd);
  for (int i = 0; i < res->count_connectors; i++) {
    connector = drmModeGetConnector(platform.drm.fd, res->connectors[i]);
    if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
      break;
    }
    drmModeFreeConnector(connector);
    connector = NULL;
  }
  if (!connector) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to get a drm connector");
    return -1;
  }

  platform.drm.connector_id = connector->connector_id;
  platform.drm.mode = connector->modes[0];
  platform.drm.crtc_id = res->crtcs[0];

  drmModeFreeConnector(connector);
  drmModeFreeResources(res);

  return 0;
}

static int init_egl () {
   EGLint major;
   EGLint minor;
   EGLConfig config = NULL;
   EGLint num_config;
   EGLint frame_buffer_config [] = {
     EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
     EGL_RED_SIZE,   8,
     EGL_GREEN_SIZE, 8,
     EGL_BLUE_SIZE,  8,
     EGL_DEPTH_SIZE, 24,
     EGL_NONE
   };
   // ask for an OpenGL ES 2 rendering context
   EGLint context_config [] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE, EGL_NONE };

   platform.gbm.device = gbm_create_device(platform.drm.fd);
   if (!platform.gbm.device) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to create gbm device");
     return -1;
   }

   platform.egl.display = eglGetDisplay(platform.gbm.device);
   if (platform.egl.display == EGL_NO_DISPLAY) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to get an EGL display");
     return -1;
   }

   if (!eglInitialize(platform.egl.display, &major, &minor)) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to initialize the EGL display. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }
   TRACELOG(LOG_INFO, "COMMA: Using EGL version %i.%i", major, minor);

   if (!eglGetConfigs(platform.egl.display, NULL, 0, &num_config) || num_config < 1) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to list EGL display configs. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }

   EGLConfig *configs = malloc(num_config * sizeof(EGLConfig));
   if (!eglChooseConfig(platform.egl.display, frame_buffer_config, configs, num_config, &num_config)) {
     TRACELOG(LOG_WARNING, "%s", eglGetErrorString(eglGetError()));
     return -1;
   }
   if (num_config == 0) {
     TRACELOG(LOG_WARNING, "%s", eglGetErrorString(eglGetError()));
     return -1;
   }

   for (int i = 0; i < num_config; ++i) {
     EGLint gbm_format;
     if (!eglGetConfigAttrib(platform.egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format)) {
       continue;
     }

     if (gbm_format == GBM_FORMAT_ABGR8888) {
       config = configs[i];
       free(configs);
       break;
     }
   }

   if (config == NULL) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to find correct config");
     return -1;
   }

   platform.gbm.surface = gbm_surface_create(platform.gbm.device, platform.drm.mode.hdisplay, platform.drm.mode.vdisplay, GBM_FORMAT_ABGR8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
   if (!platform.gbm.surface) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to create gbm surface");
     return -1;
   }

   platform.egl.surface = eglCreateWindowSurface(platform.egl.display, config, (EGLNativeWindowType)platform.gbm.surface, NULL);
   if (platform.egl.surface == EGL_NO_SURFACE) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to create an EGL surface. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }

   platform.egl.context = eglCreateContext(platform.egl.display, config, EGL_NO_CONTEXT, context_config);
   if (platform.egl.context == EGL_NO_CONTEXT) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to create an OpenGL ES context. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }

   if (!eglMakeCurrent(platform.egl.display, platform.egl.surface, platform.egl.surface, platform.egl.context)) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to attach the OpenGL ES context to the EGL surface. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }

   // >1 is not supported
   if (!eglSwapInterval(platform.egl.display, (CORE.Window.flags & FLAG_VSYNC_HINT) ? 1 : 0)) {
     TRACELOG(LOG_WARNING, "COMMA: eglSwapInterval failed. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }

   return 0;
}

static void bo_user_data_destroy(struct gbm_bo *bo, void *user_data) {
  uint32_t fb_id = (uint32_t)(uintptr_t)user_data;
  if (fb_id) {
    drmModeRmFB(platform.drm.fd, fb_id);
  }
}

static int get_or_create_fb_for_bo(struct gbm_bo *bo, uint32_t *out_fb) {
    void *user_data = gbm_bo_get_user_data(bo);
    if (user_data) {
      *out_fb = (uint32_t)(uintptr_t)user_data;
      return 0;
    }

    uint32_t w = gbm_bo_get_width(bo);
    uint32_t h = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;

    uint32_t handles[4] = { handle };
    uint32_t pitches[4] = { stride };
    uint32_t offsets[4] = { 0 };
    uint32_t fb_id = 0;

    if (drmModeAddFB2(platform.drm.fd, w, h, GBM_FORMAT_ABGR8888, handles, pitches, offsets, &fb_id, 0) != 0) {
      return -1;
    }

    gbm_bo_set_user_data(bo, (void*)(uintptr_t)fb_id, bo_user_data_destroy);

    *out_fb = fb_id;
    return 0;
}

static FILE* open_with_retry(const char *path, const char *mode) {
  const int sleep_ms = 50;
  int waited = 0;
  FILE *f = NULL;

  while (waited <= 500) {
    f = fopen(path, mode);
    if (f) {
      return f;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = sleep_ms * 1000000L };
    nanosleep(&ts, NULL);
    waited += sleep_ms;
  }

  return NULL;
}

static int turn_screen_on () {
  FILE *f = open_with_retry("/sys/class/backlight/panel0-backlight/bl_power", "w");
  if (f) {
    fputs("0", f);
    fclose(f);
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open bl_power");
    return -1;
  }

  unsigned long max_brightness = 0;
  f = open_with_retry("/sys/class/backlight/panel0-backlight/max_brightness", "r");
  if (f) {
    fscanf(f, "%lu", &max_brightness);
    fclose(f);
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open max_brightness");
    return -1;
  }

  f = open_with_retry("/sys/class/backlight/panel0-backlight/brightness", "w");
  if (f) {
    fprintf(f, "%lu", max_brightness);
    fclose(f);
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open brightness");
    return -1;
  }

  return 0;
}

static int init_screen () {
  CORE.Window.rotation_angle = platform.canonical_zero ? 270 : 90;
  CORE.Window.rotation_source = (Rectangle){0.0, 0.0, CORE.Window.screen.width, -((int)CORE.Window.screen.height)};
  CORE.Window.rotation_destination = (Rectangle){CORE.Window.screen.height/2, CORE.Window.screen.width/2, CORE.Window.screen.width, CORE.Window.screen.height};
  CORE.Window.rotation_origin = (Vector2){CORE.Window.screen.width/2, CORE.Window.screen.height/2};

  eglSwapBuffers(platform.egl.display, platform.egl.surface);

  platform.gbm.current_bo = gbm_surface_lock_front_buffer(platform.gbm.surface);
  if (!platform.gbm.current_bo) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to get initial front buffer object");
    return -1;
  }

  if (get_or_create_fb_for_bo(platform.gbm.current_bo, &platform.gbm.current_fb)) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to get initial frame buffer");
    return -1;
  }

  drmModeCrtc *crtc = drmModeGetCrtc(platform.drm.fd, platform.drm.crtc_id);
  if (!((crtc->mode_valid != 0) && (crtc->buffer_id != 0))) {
    if (drmModeSetCrtc(platform.drm.fd, platform.drm.crtc_id, platform.gbm.current_fb, 0, 0, &platform.drm.connector_id, 1, &platform.drm.mode)) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to set CRTC");
      return -1;
    }
  }

  drmModeFreeCrtc(crtc);

  if (turn_screen_on()) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to turn screen on");
    return -1;
  }

  return 0;
}

static int init_touch(const char *dev_path) {
  platform.touch.fd = open(dev_path, O_RDONLY|O_NONBLOCK);
  if (platform.touch.fd < 0) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open touch device at %s", dev_path);
    return -1;
  }

  FILE *fp = fopen("/sys/devices/platform/vendor/vendor:gpio-som-id/som_id", "r");
  if (fp != NULL) {
    int origin;
    int ret = fscanf(fp, "%d", &origin);
    fclose(fp);
    if (ret != 1) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to test for screen origin");
      return -1;
    } else {
      platform.canonical_zero = origin == 1;
    }
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open screen origin");
    platform.canonical_zero = false;
  }

  for (int i = 0; i < MAX_TOUCH_POINTS; ++i) {
    platform.touch.fingers[i].x = -1;
    platform.touch.fingers[i].y = -1;
    platform.touch.fingers[i].state = FINGER_STATE_REMOVED;
    platform.touch.fingers[i].resetNextFrame = false;

    CORE.Input.Touch.currentTouchState[0] = 0;
    CORE.Input.Touch.previousTouchState[0] = 0;
  }

  for (int i = 0; i < MAX_MOUSE_BUTTONS; ++i) {
    CORE.Input.Mouse.currentButtonState[i] = 0;
    CORE.Input.Mouse.previousButtonState[i] = 0;
  }

  CORE.Input.Mouse.currentPosition.x = -1;
  CORE.Input.Mouse.currentPosition.y = -1;
  CORE.Input.Mouse.previousPosition.x = -1;
  CORE.Input.Mouse.previousPosition.y = -1;

  return 0;
}

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
int InitPlatform(void);          // Initialize platform (graphics, inputs and more)
bool InitGraphicsDevice(void);   // Initialize graphics device

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
// NOTE: Functions declaration is provided by raylib.h

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------

// Check if application should close
bool WindowShouldClose(void) {
  return false;
}

// Toggle fullscreen mode
void ToggleFullscreen(void) {
  TRACELOG(LOG_WARNING, "ToggleFullscreen() not available on target platform");
}

// Toggle borderless windowed mode
void ToggleBorderlessWindowed(void) {
  TRACELOG(LOG_WARNING, "ToggleBorderlessWindowed() not available on target platform");
}

// Set window state: maximized, if resizable
void MaximizeWindow(void) {
  TRACELOG(LOG_WARNING, "MaximizeWindow() not available on target platform");
}

// Set window state: minimized
void MinimizeWindow(void) {
  TRACELOG(LOG_WARNING, "MinimizeWindow() not available on target platform");
}

// Set window state: not minimized/maximized
void RestoreWindow(void) {
  TRACELOG(LOG_WARNING, "RestoreWindow() not available on target platform");
}

// Set window configuration state using flags
void SetWindowState(unsigned int flags) {
  TRACELOG(LOG_WARNING, "SetWindowState() not available on target platform");
}

// Clear window configuration state flags
void ClearWindowState(unsigned int flags) {
  TRACELOG(LOG_WARNING, "ClearWindowState() not available on target platform");
}

// Set icon for window
void SetWindowIcon(Image image) {
  TRACELOG(LOG_WARNING, "SetWindowIcon() not available on target platform");
}

// Set icon for window
void SetWindowIcons(Image *images, int count) {
  TRACELOG(LOG_WARNING, "SetWindowIcons() not available on target platform");
}

// Set title for window
void SetWindowTitle(const char *title) {
  CORE.Window.title = title;
}

// Set window position on screen (windowed mode)
void SetWindowPosition(int x, int y) {
  TRACELOG(LOG_WARNING, "SetWindowPosition() not available on target platform");
}

// Set monitor for the current window
void SetWindowMonitor(int monitor) {
  TRACELOG(LOG_WARNING, "SetWindowMonitor() not available on target platform");
}

// Set window minimum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMinSize(int width, int height) {
  CORE.Window.screenMin.width = width;
  CORE.Window.screenMin.height = height;
}

// Set window maximum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMaxSize(int width, int height) {
  CORE.Window.screenMax.width = width;
  CORE.Window.screenMax.height = height;
}

// Set window dimensions
void SetWindowSize(int width, int height) {
  TRACELOG(LOG_WARNING, "SetWindowSize() not available on target platform");
}

// Set window opacity, value opacity is between 0.0 and 1.0
void SetWindowOpacity(float opacity) {
  TRACELOG(LOG_WARNING, "SetWindowOpacity() not available on target platform");
}

// Set window focused
void SetWindowFocused(void) {
  TRACELOG(LOG_WARNING, "SetWindowFocused() not available on target platform");
}

// Get native window handle
void *GetWindowHandle(void) {
  TRACELOG(LOG_WARNING, "GetWindowHandle() not implemented on target platform");
  return NULL;
}

// Get number of monitors
int GetMonitorCount(void) {
  TRACELOG(LOG_WARNING, "GetMonitorCount() not implemented on target platform");
  return 1;
}

// Get number of monitors
int GetCurrentMonitor(void) {
  TRACELOG(LOG_WARNING, "GetCurrentMonitor() not implemented on target platform");
  return 0;
}

// Get selected monitor position
Vector2 GetMonitorPosition(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorPosition() not implemented on target platform");
  return (Vector2){ 0, 0 };
}

// Get selected monitor width (currently used by monitor)
int GetMonitorWidth(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorWidth() not implemented on target platform");
  return 0;
}

// Get selected monitor height (currently used by monitor)
int GetMonitorHeight(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorHeight() not implemented on target platform");
  return 0;
}

// Get selected monitor physical width in millimetres
int GetMonitorPhysicalWidth(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorPhysicalWidth() not implemented on target platform");
  return 0;
}

// Get selected monitor physical height in millimetres
int GetMonitorPhysicalHeight(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorPhysicalHeight() not implemented on target platform");
  return 0;
}

// Get selected monitor refresh rate
int GetMonitorRefreshRate(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorRefreshRate() not implemented on target platform");
  return 0;
}

// Get the human-readable, UTF-8 encoded name of the selected monitor
const char *GetMonitorName(int monitor) {
  TRACELOG(LOG_WARNING, "GetMonitorName() not implemented on target platform");
  return "";
}

// Get window position XY on monitor
Vector2 GetWindowPosition(void) {
  TRACELOG(LOG_WARNING, "GetWindowPosition() not implemented on target platform");
  return (Vector2){ 0, 0 };
}

// Get window scale DPI factor for current monitor
Vector2 GetWindowScaleDPI(void) {
  TRACELOG(LOG_WARNING, "GetWindowScaleDPI() not implemented on target platform");
  return (Vector2){ 1.0f, 1.0f };
}

// Set clipboard text content
void SetClipboardText(const char *text) {
  TRACELOG(LOG_WARNING, "SetClipboardText() not implemented on target platform");
}

// Get clipboard text content
// NOTE: returned string is allocated and freed by GLFW
const char *GetClipboardText(void) {
  TRACELOG(LOG_WARNING, "GetClipboardText() not implemented on target platform");
  return NULL;
}

// Get clipboard image
Image GetClipboardImage(void) {
  Image image = { 0 };
  TRACELOG(LOG_WARNING, "GetClipboardImage() not implemented on target platform");
  return image;
}

// Show mouse cursor
void ShowCursor(void) {
  CORE.Input.Mouse.cursorHidden = false;
}

// Hides mouse cursor
void HideCursor(void) {
  CORE.Input.Mouse.cursorHidden = true;
}

// Enables cursor (unlock cursor)
void EnableCursor(void) {
  // Set cursor position in the middle
  SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);

  CORE.Input.Mouse.cursorHidden = false;
}

// Disables cursor (lock cursor)
void DisableCursor(void) {
  // Set cursor position in the middle
  SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);

  CORE.Input.Mouse.cursorHidden = true;
}

// Swap back buffer with front buffer (screen drawing)
void SwapScreenBuffer(void) {
  eglSwapBuffers(platform.egl.display, platform.egl.surface);

  platform.gbm.next_bo = gbm_surface_lock_front_buffer(platform.gbm.surface);
  if (!platform.gbm.next_bo) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to get rendered buffer object");
    return;
  }

  if (get_or_create_fb_for_bo(platform.gbm.next_bo, &platform.gbm.next_fb)) {
    gbm_surface_release_buffer(platform.gbm.surface, platform.gbm.next_bo);
    platform.gbm.next_bo = NULL;
    TRACELOG(LOG_WARNING, "COMMA: Failed to get frame buffer for rendered buffer object");
    return;
  }

  if (drmModePageFlip(platform.drm.fd, platform.drm.crtc_id, platform.gbm.next_fb, 0, NULL) != 0) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to page flip");
    drmModeRmFB(platform.drm.fd, platform.gbm.next_fb);
    gbm_surface_release_buffer(platform.gbm.surface, platform.gbm.next_bo);
    platform.gbm.next_bo = NULL;
    platform.gbm.next_fb = 0;
    return;
  }

  drmVBlank v = {0};
  v.request.type = DRM_VBLANK_RELATIVE;
  v.request.sequence = 1;
  drmWaitVBlank(platform.drm.fd, &v);

  if (platform.gbm.current_bo) {
    gbm_surface_release_buffer(platform.gbm.surface, platform.gbm.current_bo);
  }

  platform.gbm.current_bo = platform.gbm.next_bo;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------

// Get elapsed time measure in seconds since InitTimer()
double GetTime(void) {
  double time = 0.0;
  struct timespec ts = { 0 };
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned long long int nanoSeconds = (unsigned long long int)ts.tv_sec*1000000000LLU + (unsigned long long int)ts.tv_nsec;

  time = (double)(nanoSeconds - CORE.Time.base)*1e-9;  // Elapsed time since InitTimer()

  return time;
}

void OpenURL(const char *url) {
  TRACELOG(LOG_WARNING, "OpenURL() not implemented on target platform");
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Inputs
//----------------------------------------------------------------------------------

// Set internal gamepad mappings
int SetGamepadMappings(const char *mappings) {
  TRACELOG(LOG_WARNING, "SetGamepadMappings() not implemented on target platform");
  return 0;
}

void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration) {
  TRACELOG(LOG_WARNING, "GamepadSetVibration() not implemented on target platform");
}

// Set mouse position XY
void SetMousePosition(int x, int y) {
  CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
  CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
}

// Set mouse cursor
void SetMouseCursor(int cursor) {
  TRACELOG(LOG_WARNING, "SetMouseCursor() not implemented on target platform");
}

// Get physical key name.
const char *GetKeyName(int key) {
  TRACELOG(LOG_WARNING, "GetKeyName() not implemented on target platform");
  return "";
}

void PollInputEvents(void) {
  // slot i is for events of finger i
  static int slot = 0;

  for (int i = 0; i < MAX_TOUCH_POINTS; ++i) {
    CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];
    // caused by single frame down and up events
    if (platform.touch.fingers[i].resetNextFrame) {
      CORE.Input.Touch.currentTouchState[i] = 0;
      platform.touch.fingers[i].resetNextFrame = false;
    }
  }

  for (int i = 0; i < MAX_MOUSE_BUTTONS; ++i) {
    CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
  }

  CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
  CORE.Input.Touch.pointCount = 0;

  struct input_event event = {0};
  while (read(platform.touch.fd, &event, sizeof(struct input_event)) == sizeof(struct input_event)) {
    if (event.type == SYN_REPORT) { // synchronization frame. Expose completed events back to the library

      for (int i = 0; i < MAX_TOUCH_POINTS; ++i) {
        if (platform.touch.fingers[i].state == FINGER_STATE_TOUCHING) {

          CORE.Input.Touch.position[i].x = platform.touch.fingers[i].x;
          CORE.Input.Touch.position[i].y = platform.touch.fingers[i].y;
          CORE.Input.Touch.currentTouchState[i] = 1;

          // map main finger on mouse for conveniance. raylib already does that
          // for pressed state, but not pos
          if (i == 0) {
            CORE.Input.Mouse.currentPosition.x = platform.touch.fingers[i].x;
            CORE.Input.Mouse.currentPosition.y = platform.touch.fingers[i].y;
          }

        } else if (platform.touch.fingers[i].state == FINGER_STATE_REMOVING) {
          // if we received a touch down and up event in the same frame,
          // delay up event by one frame so that API user needs no special handling
          if (CORE.Input.Touch.previousTouchState[i] == 0) {
            CORE.Input.Touch.currentTouchState[i] = 1;
            platform.touch.fingers[i].resetNextFrame = true;  // mark to be reset next event update loop
          } else {
            CORE.Input.Touch.currentTouchState[i] = 0;
          }

          platform.touch.fingers[i].state = FINGER_STATE_REMOVED;
        }
      }

    } else if (event.type == EV_ABS) { // raw events. Process these untill we get a sync frame

      if (event.code == ABS_MT_SLOT) { // switch finger
        slot = event.value;
      } else if (event.code == ABS_MT_TRACKING_ID) { // finger on screen or not
        platform.touch.fingers[slot].state = event.value == -1 ? FINGER_STATE_REMOVING : FINGER_STATE_TOUCHING;
      } else if (event.code == ABS_MT_POSITION_X) {
        platform.touch.fingers[slot].y = (1 - platform.canonical_zero) * (CORE.Window.screen.height - event.value) + (platform.canonical_zero * event.value);
      } else if (event.code == ABS_MT_POSITION_Y) {
        platform.touch.fingers[slot].x = platform.canonical_zero * (CORE.Window.screen.width - event.value) + ((1 - platform.canonical_zero) * event.value);
      }
    }
  }

  // count how many fingers are left on the screen after processing all events
  for (int i = 0; i < MAX_TOUCH_POINTS; ++i) {
    CORE.Input.Touch.pointCount += platform.touch.fingers[i].state == FINGER_STATE_TOUCHING;
  }
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------

int InitPlatform(void) {
  // only support fullscreen
  CORE.Window.fullscreen = true;
  CORE.Window.flags |= FLAG_FULLSCREEN_MODE;

  if (init_drm("/dev/dri/card0")) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize drm");
    return -1;
  }

  CORE.Window.screen.width = platform.drm.mode.vdisplay;
  CORE.Window.screen.height = platform.drm.mode.hdisplay;

  CORE.Window.display.width = CORE.Window.screen.width;
  CORE.Window.display.height = CORE.Window.screen.height;

  // swapped since we render in landscape mode
  CORE.Window.currentFbo.width = CORE.Window.screen.height;
  CORE.Window.currentFbo.height = CORE.Window.screen.width;

  if (init_egl()) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize EGL");
    return -1;
  }

  if (init_touch("/dev/input/event2")) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize touch device");
    return -1;
  }

  if (init_screen()) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize screen");
    return -1;
  }

  if (init_color_correction()) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to initialize color correction");
  }

  SetupFramebuffer(CORE.Window.currentFbo.width, CORE.Window.currentFbo.height);
  rlLoadExtensions(eglGetProcAddress);
  InitTimer();
  CORE.Storage.basePath = GetWorkingDirectory();

  CORE.Window.ready = true;

  TRACELOG(LOG_INFO, "COMMA: Initialized successfully");
  return 0;
}

void ClosePlatform(void) {
  CORE.Window.ready = false;

  if (platform.egl.display != EGL_NO_DISPLAY) {
    eglMakeCurrent(platform.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (platform.egl.surface != EGL_NO_SURFACE) {
      eglDestroySurface(platform.egl.display, platform.egl.surface);
      platform.egl.surface = EGL_NO_SURFACE;
    }
    if (platform.egl.context != EGL_NO_CONTEXT) {
      eglDestroyContext(platform.egl.display, platform.egl.context);
      platform.egl.context = EGL_NO_CONTEXT;
    }
    eglTerminate(platform.egl.display);
    platform.egl.display = EGL_NO_DISPLAY;
  }

  if (platform.gbm.surface && platform.gbm.next_bo) {
    gbm_surface_release_buffer(platform.gbm.surface, platform.gbm.next_bo);
  }

  if (platform.gbm.device) {
    gbm_device_destroy(platform.gbm.device);
    platform.gbm.device = NULL;
  }

  close(platform.touch.fd);
}
