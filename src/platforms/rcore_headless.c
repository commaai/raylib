/**********************************************************************************************
*
*   rcore_headless - Functions to manage window, graphics device and inputs
*
*   PLATFORM: HEADLESS
*       - Offscreen rendering without any display server (no X11, Wayland, DRM/KMS)
*       - EGL surfaceless platform (Mesa) with a pbuffer surface, OpenGL ES 2.0+
*
*   LIMITATIONS:
*       - No window is ever shown; window flags are accepted but ignored
*       - No input devices; input state can only be set programmatically
*         (e.g. SetMousePosition()) or injected above raylib
*
*   DEPENDENCIES:
*       - EGL and OpenGL ES 2.0 libraries (Mesa llvmpipe works, no GPU required)
*
*   LICENSE: zlib/libpng
*
*   Copyright (c) 2013-2026 Ramon Santamaria (@raysan5) and contributors
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
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
  #define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

typedef struct {
  EGLDisplay display;
  EGLSurface surface;   // pbuffer sized to the requested window
  EGLContext context;
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { 0 };   // Platform specific data

//----------------------------------------------------------------------------------
// Headless specific code
//----------------------------------------------------------------------------------

#define CASE_STR( value ) case value: return #value;
static const char *eglGetErrorString(EGLint error) {
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

static EGLDisplay get_egl_display(void) {
  // Prefer the Mesa surfaceless platform: works with no display server and no
  // DRM device node (llvmpipe), which is exactly the CI environment
  const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (client_exts && strstr(client_exts, "EGL_EXT_platform_base") && strstr(client_exts, "EGL_MESA_platform_surfaceless")) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay) {
      EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
      if (display != EGL_NO_DISPLAY) return display;
    }
  }

  // Fallback for non-Mesa stacks (honors the EGL_PLATFORM env var)
  return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static int init_egl(void) {
  EGLint major, minor;
  EGLConfig config = NULL;
  EGLint num_config = 0;
  EGLint frame_buffer_config[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,     8,
    EGL_GREEN_SIZE,   8,
    EGL_BLUE_SIZE,    8,
    EGL_ALPHA_SIZE,   8,
    EGL_DEPTH_SIZE,   24,
    EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  // ask for an OpenGL ES 2 rendering context; drivers return the highest
  // backward-compatible version (e.g. ES 3.2 on Mesa)
  EGLint context_config[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLint pbuffer_config[] = {
    EGL_WIDTH, (EGLint)CORE.Window.screen.width,
    EGL_HEIGHT, (EGLint)CORE.Window.screen.height,
    EGL_NONE
  };

  platform.display = get_egl_display();
  if (platform.display == EGL_NO_DISPLAY) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to get an EGL display");
    return -1;
  }

  if (!eglInitialize(platform.display, &major, &minor)) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to initialize the EGL display. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }
  TRACELOG(LOG_INFO, "HEADLESS: Using EGL version %i.%i", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to bind the OpenGL ES API. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }

  if (!eglChooseConfig(platform.display, frame_buffer_config, &config, 1, &num_config) || num_config < 1) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to choose an EGL config. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }

  platform.surface = eglCreatePbufferSurface(platform.display, config, pbuffer_config);
  if (platform.surface == EGL_NO_SURFACE) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to create an EGL pbuffer surface. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }

  platform.context = eglCreateContext(platform.display, config, EGL_NO_CONTEXT, context_config);
  if (platform.context == EGL_NO_CONTEXT) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to create an OpenGL ES context. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }

  if (!eglMakeCurrent(platform.display, platform.surface, platform.surface, platform.context)) {
    TRACELOG(LOG_WARNING, "HEADLESS: Failed to attach the OpenGL ES context to the EGL surface. Error code: %s", eglGetErrorString(eglGetError()));
    return -1;
  }

  return 0;
}

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
int InitPlatform(void);          // Initialize platform (graphics, inputs and more)
bool InitGraphicsDevice(void);   // Initialize graphics device

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
  return 1;
}

// Get number of monitors
int GetCurrentMonitor(void) {
  return 0;
}

// Get selected monitor position
Vector2 GetMonitorPosition(int monitor) {
  return (Vector2){ 0, 0 };
}

// Get selected monitor width (currently used by monitor)
int GetMonitorWidth(int monitor) {
  return 0;
}

// Get selected monitor height (currently used by monitor)
int GetMonitorHeight(int monitor) {
  return 0;
}

// Get selected monitor physical width in millimetres
int GetMonitorPhysicalWidth(int monitor) {
  return 0;
}

// Get selected monitor physical height in millimetres
int GetMonitorPhysicalHeight(int monitor) {
  return 0;
}

// Get selected monitor refresh rate
int GetMonitorRefreshRate(int monitor) {
  return 0;
}

// Get the human-readable, UTF-8 encoded name of the selected monitor
const char *GetMonitorName(int monitor) {
  return "headless";
}

// Get window position XY on monitor
Vector2 GetWindowPosition(void) {
  return (Vector2){ 0, 0 };
}

// Get window scale DPI factor for current monitor
Vector2 GetWindowScaleDPI(void) {
  return (Vector2){ 1.0f, 1.0f };
}

// Set clipboard text content
void SetClipboardText(const char *text) {
  TRACELOG(LOG_WARNING, "SetClipboardText() not implemented on target platform");
}

// Get clipboard text content
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
  // eglSwapBuffers() is defined to be a no-op on pbuffer surfaces, but it still
  // marks the frame boundary for the driver
  eglSwapBuffers(platform.display, platform.surface);
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
  TRACELOG(LOG_WARNING, "SetGamepadVibration() not implemented on target platform");
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

// Register all input events
// There are no input devices; only rotate state so programmatically injected
// input (SetMousePosition() or state set above raylib) behaves consistently
void PollInputEvents(void) {
#if SUPPORT_GESTURES_SYSTEM
  UpdateGestures();
#endif

  // Reset keys/chars pressed registered
  CORE.Input.Keyboard.keyPressedQueueCount = 0;
  CORE.Input.Keyboard.charPressedQueueCount = 0;

  // Reset last gamepad button/axis registered state
  CORE.Input.Gamepad.lastButtonPressed = 0;   // GAMEPAD_BUTTON_UNKNOWN

  // Register previous keys states
  for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) {
    CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
    CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
  }

  // Register previous mouse states
  CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
  CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
  CORE.Input.Mouse.currentWheelMove = (Vector2){ 0.0f, 0.0f };
  for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
    CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
  }

  // Register previous touch states
  for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
    CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];
  }
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------

int InitPlatform(void) {
  if (CORE.Window.screen.width == 0) CORE.Window.screen.width = 640;
  if (CORE.Window.screen.height == 0) CORE.Window.screen.height = 480;

  CORE.Window.display.width = CORE.Window.screen.width;
  CORE.Window.display.height = CORE.Window.screen.height;
  CORE.Window.render.width = CORE.Window.screen.width;
  CORE.Window.render.height = CORE.Window.screen.height;
  CORE.Window.currentFbo.width = CORE.Window.render.width;
  CORE.Window.currentFbo.height = CORE.Window.render.height;
  CORE.Window.renderOffset.x = 0;
  CORE.Window.renderOffset.y = 0;

  if (init_egl()) {
    TRACELOG(LOG_FATAL, "HEADLESS: Failed to initialize EGL");
    return -1;
  }

  rlLoadExtensions(eglGetProcAddress);
  TRACELOG(LOG_INFO, "HEADLESS: OpenGL version: %s", glGetString(GL_VERSION));

  InitTimer();
  CORE.Storage.basePath = GetWorkingDirectory();

  CORE.Window.ready = true;

  TRACELOG(LOG_INFO, "HEADLESS: Initialized successfully");
  return 0;
}

void ClosePlatform(void) {
  CORE.Window.ready = false;

  if (platform.display != EGL_NO_DISPLAY) {
    eglMakeCurrent(platform.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (platform.surface != EGL_NO_SURFACE) {
      eglDestroySurface(platform.display, platform.surface);
      platform.surface = EGL_NO_SURFACE;
    }
    if (platform.context != EGL_NO_CONTEXT) {
      eglDestroyContext(platform.display, platform.context);
      platform.context = EGL_NO_CONTEXT;
    }
    eglTerminate(platform.display);
    platform.display = EGL_NO_DISPLAY;
  }

  // fully release EGL thread state so init -> close -> init works
  eglReleaseThread();
}
