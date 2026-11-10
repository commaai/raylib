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


// hold all the low level drm stuff
struct drm_platform {
  int fd;
  int render_fd;
  int socket_fd;

  uint32_t connector_id;
  uint32_t crtc_id;
  bool needs_modeset;

  drmModeModeInfo mode;
};


typedef struct {
    struct touch touch;
    struct drm_platform drm;
    bool canonical_zero;
    bool debug_mode;
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { .drm = { .fd = -1, .render_fd = -1, .socket_fd = -1 }, .touch = { .fd = -1 } };   // Platform specific data

//----------------------------------------------------------------------------------
// comma specific code
//----------------------------------------------------------------------------------

// These are actually float16s, so need to be converted before use
struct __attribute__((__packed__)) color_correction_values {
  uint16_t gamma;
  uint16_t ccm[9];
  uint16_t rgb_color_gains[3];
};


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

#include "rcore_comma_display.c"

// the blobs of the display's degamma, matrix and gamma blocks, as msm_drm_pp.h's drm_msm_igc_lut, drm_msm_pcc and
// drm_msm_pgc_lut: flags first, and padded to 8 bytes
typedef struct {
  uint64_t flags;
  uint32_t lut[3][256];  // per channel, 12 bit
  uint32_t strength;
} igc_blob;

typedef struct {
  int32_t c, r, g, b, rg, gb, rb, rgb;  // an output channel's terms, in 1/32768
} pcc_coefficients;

typedef struct {
  uint64_t flags;
  pcc_coefficients channels[3];
  int32_t second_order[9];
} pcc_blob;

typedef struct {
  uint64_t flags;
  uint32_t lut[3][512];  // per channel, two 10 bit entries per word
} gc_blob;

// the display's color calibration, in its own degamma, matrix and gamma blocks
static void set_color_correction(bool enable) {
  static const char *names[3] = {"SDE_DSPP_IGC_V3", "SDE_DSPP_PCC_V4", "SDE_DSPP_GC_V1"};
  static igc_blob igc;
  static pcc_blob pcc;
  static gc_blob gc;
  struct color_correction_values *ccv = enable && !platform.canonical_zero ? read_correction_values() : NULL;
  float gamma = 0.0f;
  float ccm[9];
  float gains[3];
  bool valid = ccv != NULL;
  if (ccv) {
    gamma = decode_float16(ccv->gamma);
    valid = isfinite(gamma) && gamma > 0.0f;
    for (int i = 0; i < 9; i++) {
      ccm[i] = decode_float16(ccv->ccm[i]);
      valid = valid && isfinite(ccm[i]);
    }
    for (int i = 0; i < 3; i++) {
      gains[i] = decode_float16(ccv->rgb_color_gains[i]);
      valid = valid && isfinite(gains[i]) && gains[i] > 0.0f;
    }
    free(ccv);
  }
  if (!valid) {
    for (int i = 0; i < 3; i++) set_crtc_blob(names[i], NULL, 0);
    return;
  }
  for (int c = 0; c < 3; c++) {
    for (int i = 0; i < 256; i++) igc.lut[c][i] = (uint32_t)roundf(powf(i / 255.0f, 2.2f) * 4095.0f);
    for (int i = 0; i < 512; i++) {
      uint32_t lo = (uint32_t)roundf(powf((2*i) / 1023.0f, 1.0f / gamma / 2.2f) * 1023.0f);
      uint32_t hi = (uint32_t)roundf(powf((2*i + 1) / 1023.0f, 1.0f / gamma / 2.2f) * 1023.0f);
      gc.lut[c][i] = lo | hi << 16;
    }
    pcc_coefficients *out = &pcc.channels[c];
    out->r = (int32_t)roundf(ccm[0*3 + c] / gains[0] * 32768.0f);
    out->g = (int32_t)roundf(ccm[1*3 + c] / gains[1] * 32768.0f);
    out->b = (int32_t)roundf(ccm[2*3 + c] / gains[2] * 32768.0f);
  }
  const void *blobs[3] = {&igc, &pcc, &gc};
  const uint32_t sizes[3] = {sizeof(igc), sizeof(pcc), sizeof(gc)};
  for (int i = 0; i < 3; i++) {
    int ret = set_crtc_blob(names[i], blobs[i], sizes[i]);
    if (ret) TRACELOG(LOG_WARNING, "COMMA: Failed to set %s: %s", names[i], strerror(-ret));
  }
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

  if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) < 0) {
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
    platform.drm.fd = fcntl((int)v, F_DUPFD_CLOEXEC, 0);
  } else if (getenv("NO_MASTER")) {
    platform.drm.fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  } else {
    const char *sock_path = "/tmp/drmfd.sock";
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
      close(s);
      return -1;
    }

    platform.drm.socket_fd = s;
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

  // Keep GEM handles and framebuffers on a process-owned DRM file. The
  // broker's master file outlives us, so objects created there survive crashes.
  platform.drm.render_fd = open(dev_path, O_RDWR | O_CLOEXEC);
  drm_magic_t magic;
  if (platform.drm.render_fd < 0 ||
      drmGetMagic(platform.drm.render_fd, &magic) != 0 ||
      drmAuthMagic(platform.drm.fd, magic) != 0) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to authenticate render device");
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
    if (fscanf(f, "%lu", &max_brightness) != 1) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to read max_brightness");
      fclose(f);
      return -1;
    }
    fclose(f);
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open max_brightness");
    return -1;
  }

  f = open_with_retry("/sys/class/backlight/panel0-backlight/brightness", "w");
  if (f) {
    max_brightness = (int)((max_brightness / 100.0 ) * 65.0);
    fprintf(f, "%lu", max_brightness);
    fclose(f);
  } else {
    TRACELOG(LOG_WARNING, "COMMA: Failed to open brightness");
    return -1;
  }

  return 0;
}

// the first frame sets the CRTC again if it's off
static void check_crtc(void) {
  drmModeCrtc *crtc = drmModeGetCrtc(platform.drm.fd, platform.drm.crtc_id);
  platform.drm.needs_modeset = !crtc || !crtc->mode_valid || !crtc->buffer_id;
  drmModeFreeCrtc(crtc);
}

static int init_screen(void) {
  int ret = display_init();
  if (ret) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to initialize the display: %s", strerror(-ret));
    return -1;
  }
  check_crtc();
  return turn_screen_on();
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

// show a framebuffer at the next vblank, and wait for it
static bool present_framebuffer(uint32_t framebuffer) {
  // Closing another client's DRM file can remove the active framebuffer and
  // disable the CRTC. Restore it when page flipping is no longer possible.
  if (platform.drm.needs_modeset ||
      drmModePageFlip(platform.drm.fd, platform.drm.crtc_id, framebuffer, 0, NULL) != 0) {
    if (!platform.drm.needs_modeset) TRACELOG(LOG_WARNING, "COMMA: Failed to page flip");
    if (drmModeSetCrtc(platform.drm.fd, platform.drm.crtc_id, framebuffer, 0, 0, &platform.drm.connector_id, 1, &platform.drm.mode) != 0) {
      TRACELOG(LOG_WARNING, "COMMA: Failed to set CRTC");
      platform.drm.needs_modeset = true;
      return false;
    }
    platform.drm.needs_modeset = false;
  }

  static uint32_t vblank_id = 0;
  drmVBlank v = {0};
  v.request.type = DRM_VBLANK_RELATIVE;
  v.request.sequence = 1;
  drmWaitVBlank(platform.drm.fd, &v);
  if (platform.debug_mode) {
    if ((v.reply.sequence - vblank_id) > 1) {
      TRACELOG(LOG_WARNING, "%i FRAME(s) DROPPED!", (v.reply.sequence - vblank_id) - 1);
    }
  }
  vblank_id = v.reply.sequence;
  return true;
}

// Swap back buffer with front buffer (screen drawing)
// rlsw's own framebuffer pixels, while it draws straight into the display's target instead
static void *rlsw_pixels;
// the CRTC only takes color correction once it shows a frame
static bool color_corrected;

void SwapScreenBuffer(void) {
  sw_texture_t *screen = &RLSW.framebuffer.color;
  uint8_t *target = display_target(screen->width, screen->height);
  if (!target) {
    TRACELOG(LOG_WARNING, "COMMA: Framebuffer doesn't fit the display");
    return;
  }
  if (screen->pixels != target) {
    // from now on rlsw draws into the display's target, until ReturnScreenPixels
    memcpy(target, screen->pixels, (size_t)screen->width * screen->height * 4);
    rlsw_pixels = screen->pixels;
    screen->pixels = target;
  }

  uint32_t framebuffer;
  int ret = display_rotate(&framebuffer);
  if (ret) {
    TRACELOG(LOG_WARNING, "COMMA: Failed to rotate the frame: %s", strerror(-ret));
    return;
  }
  if (!present_framebuffer(framebuffer)) return;
  display_shown();
  if (!color_corrected) {
    set_color_correction(true);
    color_corrected = true;
  }
}

// rlsw is about to resize or free its framebuffer: give it its own pixels back
static void ReturnScreenPixels(void) {
  if (!rlsw_pixels) return;
  RLSW.framebuffer.color.pixels = rlsw_pixels;
  rlsw_pixels = NULL;
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
  FLAG_SET(CORE.Window.flags, FLAG_FULLSCREEN_MODE);

  if (init_drm("/dev/dri/card0")) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize drm");
    return -1;
  }

  CORE.Window.screen.width = platform.drm.mode.vdisplay;
  CORE.Window.screen.height = platform.drm.mode.hdisplay;

  CORE.Window.display.width = CORE.Window.screen.width;
  CORE.Window.display.height = CORE.Window.screen.height;

  // drawn in landscape, the display's rotator turns it to the panel
  CORE.Window.currentFbo.width = CORE.Window.screen.width;
  CORE.Window.currentFbo.height = CORE.Window.screen.height;
  CORE.Window.render.width = CORE.Window.currentFbo.width;
  CORE.Window.render.height = CORE.Window.currentFbo.height;
  CORE.Window.renderOffset.x = 0;
  CORE.Window.renderOffset.y = 0;


  if (init_touch("/dev/input/event2")) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize touch device");
    return -1;
  }

  if (init_screen()) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize screen");
    return -1;
  }


  const char *d = getenv("MAGIC_DEBUG");
  platform.debug_mode = (d && d[0] == '1');

  InitTimer();
  CORE.Storage.basePath = GetWorkingDirectory();

  CORE.Window.ready = true;

  TRACELOG(LOG_INFO, "COMMA: Initialized successfully");
  return 0;
}

void ClosePlatform(void) {
  CORE.Window.ready = false;

  if (platform.drm.fd >= 0) {
    // Calibration belongs to the shared display, including magic after this client exits.
    color_corrected = false;
    display_close();
  }

  if (platform.drm.render_fd >= 0) close(platform.drm.render_fd);
  if (platform.drm.fd >= 0) close(platform.drm.fd);
  if (platform.drm.socket_fd >= 0) close(platform.drm.socket_fd);
  platform.drm.render_fd = platform.drm.fd = platform.drm.socket_fd = -1;
  close(platform.touch.fd);
  platform.touch.fd = -1;
  platform.drm.needs_modeset = true;
}
