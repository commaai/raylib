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
  EGLConfig config;

  EGLNativeDisplayType native_display;
  EGLNativeWindowType native_window;

  int native_window_width;
  int native_window_height;
};

typedef struct {
    struct egl_platform egl;
    struct touch touch;
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

static int init_magic () {
  return 0;
}

static int init_egl () {
   EGLint major;
   EGLint minor;
   EGLConfig config;
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

   // get an egl display with our native display (in our case, a wl_display)
   platform.egl.display = eglGetDisplay(platform.egl.native_display);
   if (platform.egl.display == EGL_NO_DISPLAY) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to get an EGL display");
     return -1;
   }

   if (!eglInitialize(platform.egl.display, &major, &minor)) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to initialize the EGL display. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }
   TRACELOG(LOG_INFO, "COMMA: Using EGL version %i.%i", major, minor);

   if (!eglChooseConfig(platform.egl.display, frame_buffer_config, &config, 1, &num_config)) {
     TRACELOG(LOG_WARNING, "COMMA: Failed to get a valid EGL display config. Error code: %s", eglGetErrorString(eglGetError()));
     return -1;
   }
   TRACELOG(LOG_INFO, "COMMA: Found %i valid EGL display configs", num_config);

   platform.egl.surface = eglCreateWindowSurface(platform.egl.display, config, platform.egl.native_window, NULL);
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

   // enable depth testing. Not necessary if only doing 2D
   glEnable(GL_DEPTH_TEST);

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
    return -1;
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

  // in our case, all those width/height are the same
  CORE.Window.currentFbo.width = CORE.Window.screen.width;
  CORE.Window.currentFbo.height = CORE.Window.screen.height;
  CORE.Window.display.width = CORE.Window.screen.width;
  CORE.Window.display.height = CORE.Window.screen.height;
  CORE.Window.render.width = CORE.Window.screen.width;
  CORE.Window.render.height = CORE.Window.screen.height;

  if (init_magic()) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize EGL");
    return -1;
  }

  if (init_touch("/dev/input/event2")) {
    TRACELOG(LOG_FATAL, "COMMA: Failed to initialize touch device");
    return -1;
  }

  SetupFramebuffer(CORE.Window.display.width, CORE.Window.display.height);
  rlLoadExtensions(eglGetProcAddress);
  InitTimer();
  CORE.Storage.basePath = GetWorkingDirectory();

  TRACELOG(LOG_INFO, "COMMA: Initialized successfully");
  return 0;
}

void ClosePlatform(void) {
}
