#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/vector.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

namespace nb = nanobind;

// Hold a buffer export until the C call returns (including exceptions).
struct RaylibBuffer {
  Py_buffer view{};
  explicit RaylibBuffer(nb::handle value) {
    if (PyObject_GetBuffer(value.ptr(), &view, PyBUF_CONTIG_RO | PyBUF_FORMAT) != 0) throw nb::python_error();
  }
  ~RaylibBuffer() { PyBuffer_Release(&view); }
  RaylibBuffer(const RaylibBuffer &) = delete;
  RaylibBuffer &operator=(const RaylibBuffer &) = delete;
};

// Bound dimensions before raylib's signed-int pixel-size arithmetic.
inline int checked_pixel_size(int width, int height, int format) {
  if (width <= 0 || height <= 0 || size_t(width) * size_t(height) > size_t(std::numeric_limits<int>::max()) / 16 ||
      format < PIXELFORMAT_UNCOMPRESSED_GRAYSCALE || format > PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA) {
    throw nb::value_error("invalid image dimensions or pixel format");
  }
  return GetPixelDataSize(width, height, format);
}

inline const Image &checked_image(nb::handle value) {
  const auto &image = nb::cast<const Image &>(value);
  const int size = checked_pixel_size(image.width, image.height, image.format);
  if (nb::hasattr(value, "_buffer")) {
    if (image.mipmaps != 1) throw nb::value_error("Python-backed images must have exactly one mipmap level");
    auto capsule = nb::cast<nb::capsule>(value.attr("_buffer"));
    auto *view = static_cast<Py_buffer *>(capsule.data());
    if (size > view->len) throw nb::value_error("image dimensions exceed its Python buffer");
  }
  return image;
}

inline int uniform_components(int type) {
  switch (type) {
    case SHADER_UNIFORM_FLOAT: case SHADER_UNIFORM_INT: case SHADER_UNIFORM_UINT: case SHADER_UNIFORM_SAMPLER2D: return 1;
    case SHADER_UNIFORM_VEC2: case SHADER_UNIFORM_IVEC2: case SHADER_UNIFORM_UIVEC2: return 2;
    case SHADER_UNIFORM_VEC3: case SHADER_UNIFORM_IVEC3: case SHADER_UNIFORM_UIVEC3: return 3;
    case SHADER_UNIFORM_VEC4: case SHADER_UNIFORM_IVEC4: case SHADER_UNIFORM_UIVEC4: return 4;
    default: throw nb::value_error("unknown shader uniform type");
  }
}

inline void shader_value(Shader shader, int location, nb::handle value, int type, int count) {
  if (count < 0) throw nb::value_error("uniform count must be nonnegative");
  const int components = uniform_components(type);
  if (type == SHADER_UNIFORM_VEC2 && count == 1 && nb::isinstance<Vector2>(value)) {
    auto &v = nb::cast<Vector2 &>(value);
    SetShaderValueV(shader, location, &v, type, count);
    return;
  }
  RaylibBuffer buffer(value);
  const bool floating = type >= SHADER_UNIFORM_FLOAT && type <= SHADER_UNIFORM_VEC4;
  const char expected = floating ? 'f' : (type >= SHADER_UNIFORM_UINT && type <= SHADER_UNIFORM_UIVEC4 ? 'I' : 'i');
  if (buffer.view.itemsize != 4 || !buffer.view.format || std::strcmp(buffer.view.format, std::string(1, expected).c_str()) != 0) {
    throw nb::type_error("shader values require a native 32-bit float, signed int, or unsigned int buffer matching the uniform type");
  }
  const size_t required = size_t(count) * size_t(components) * 4;
  if (size_t(buffer.view.len) < required) throw nb::value_error("shader value buffer is too small");
  SetShaderValueV(shader, location, buffer.view.buf, type, count);
}

// Keep one callback reference for the lifetime of the native registration. Do not
// destroy a static nb::object after Python has shut down.
inline PyObject *raylib_trace_callback = nullptr;
inline void clear_trace_log_callback() {
  SetTraceLogCallback(nullptr);
  Py_CLEAR(raylib_trace_callback);
}

inline void raylib_trace(int level, const char *format, va_list args) {
  if (!Py_IsInitialized()) return;
  nb::gil_scoped_acquire gil;
  if (!raylib_trace_callback) return;
  va_list size_args;
  va_copy(size_args, args);
  const int size = std::vsnprintf(nullptr, 0, format, size_args);
  va_end(size_args);
  std::string message;
  if (size < 0) {
    message = format;
  } else {
    std::vector<char> text(size_t(size) + 1);
    va_list render_args;
    va_copy(render_args, args);
    std::vsnprintf(text.data(), text.size(), format, render_args);
    va_end(render_args);
    message.assign(text.data(), size_t(size));
  }
  try {
    nb::borrow<nb::object>(raylib_trace_callback)(level, message);
  } catch (nb::python_error &e) {
    e.discard_as_unraisable("raylib trace callback");
  }
}

inline void check_point_count(size_t size, int count) {
  if (count < 0 || size_t(count) > size) throw nb::value_error("point count exceeds the supplied points");
}

inline std::vector<Vector2> raylib_points(nb::sequence values) {
  std::vector<Vector2> points;
  points.reserve(nb::len(values));
  for (nb::handle value : values) {
    if (nb::isinstance<Vector2>(value)) {
      points.push_back(nb::cast<Vector2>(value));
    } else {
      auto pair = nb::cast<std::array<float, 2>>(value);
      points.push_back({pair[0], pair[1]});
    }
  }
  return points;
}

inline void bind_functions(nb::module_ &m) {
  m.def("init_window", &InitWindow, nb::arg("width"), nb::arg("height"), nb::arg("title"), nb::call_guard<nb::gil_scoped_release>());
  m.def("close_window", &CloseWindow, nb::call_guard<nb::gil_scoped_release>());
  m.def("window_should_close", &WindowShouldClose);
  m.def("is_window_ready", &IsWindowReady);
  m.def("get_monitor_width", &GetMonitorWidth, nb::arg("monitor"));
  m.def("get_monitor_height", &GetMonitorHeight, nb::arg("monitor"));
  m.def("clear_background", &ClearBackground, nb::arg("color"));
  m.def("begin_drawing", &BeginDrawing);
  // Presenting a frame may wait for vsync/FPS limits. Keep input polling responsive.
  m.def("end_drawing", &EndDrawing, nb::call_guard<nb::gil_scoped_release>());
  m.def("begin_texture_mode", &BeginTextureMode, nb::arg("target"));
  m.def("end_texture_mode", &EndTextureMode);
  m.def("begin_shader_mode", &BeginShaderMode, nb::arg("shader"));
  m.def("end_shader_mode", &EndShaderMode);
  m.def("begin_scissor_mode", &BeginScissorMode, nb::arg("x"), nb::arg("y"), nb::arg("width"), nb::arg("height"));
  m.def("end_scissor_mode", &EndScissorMode);
  m.def("get_shader_location", &GetShaderLocation, nb::arg("shader"), nb::arg("uniformName"));
  m.def("set_shader_value_matrix", &SetShaderValueMatrix, nb::arg("shader"), nb::arg("locIndex"), nb::arg("mat"));
  m.def("set_shader_value_texture", &SetShaderValueTexture, nb::arg("shader"), nb::arg("locIndex"), nb::arg("texture"));
  m.def("unload_shader", &UnloadShader, nb::arg("shader"), nb::call_guard<nb::gil_scoped_release>());
  m.def("set_target_fps", &SetTargetFPS, nb::arg("fps"));
  m.def("get_frame_time", &GetFrameTime);
  m.def("get_time", &GetTime);
  m.def("get_fps", &GetFPS);
  m.def("poll_input_events", &PollInputEvents);
  m.def("set_config_flags", &SetConfigFlags, nb::arg("flags"));
  m.def("set_trace_log_level", &SetTraceLogLevel, nb::arg("logLevel"));
  m.def("is_key_pressed", &IsKeyPressed, nb::arg("key"));
  m.def("is_key_down", &IsKeyDown, nb::arg("key"));
  m.def("is_key_released", &IsKeyReleased, nb::arg("key"));
  m.def("get_key_pressed", &GetKeyPressed);
  m.def("get_char_pressed", &GetCharPressed);
  m.def("is_mouse_button_pressed", &IsMouseButtonPressed, nb::arg("button"));
  m.def("is_mouse_button_down", &IsMouseButtonDown, nb::arg("button"));
  m.def("is_mouse_button_released", &IsMouseButtonReleased, nb::arg("button"));
  m.def("get_mouse_position", &GetMousePosition);
  m.def("set_mouse_scale", &SetMouseScale, nb::arg("scaleX"), nb::arg("scaleY"));
  m.def("get_mouse_wheel_move", &GetMouseWheelMove);
  m.def("get_touch_position", &GetTouchPosition, nb::arg("index"));
  m.def("draw_line", &DrawLine, nb::arg("startPosX"), nb::arg("startPosY"), nb::arg("endPosX"), nb::arg("endPosY"), nb::arg("color"));
  m.def("draw_line_ex", &DrawLineEx, nb::arg("startPos"), nb::arg("endPos"), nb::arg("thick"), nb::arg("color"));
  m.def("draw_circle", &DrawCircle, nb::arg("centerX"), nb::arg("centerY"), nb::arg("radius"), nb::arg("color"));
  m.def("draw_circle_v", &DrawCircleV, nb::arg("center"), nb::arg("radius"), nb::arg("color"));
  m.def("draw_circle_gradient", &DrawCircleGradient, nb::arg("center"), nb::arg("radius"), nb::arg("inner"), nb::arg("outer"));
  m.def("draw_ring", &DrawRing, nb::arg("center"), nb::arg("innerRadius"), nb::arg("outerRadius"), nb::arg("startAngle"), nb::arg("endAngle"), nb::arg("segments"), nb::arg("color"));
  m.def("draw_rectangle", &DrawRectangle, nb::arg("posX"), nb::arg("posY"), nb::arg("width"), nb::arg("height"), nb::arg("color"));
  m.def("draw_rectangle_rec", &DrawRectangleRec, nb::arg("rec"), nb::arg("color"));
  m.def("draw_rectangle_gradient_v", &DrawRectangleGradientV, nb::arg("posX"), nb::arg("posY"), nb::arg("width"), nb::arg("height"), nb::arg("top"), nb::arg("bottom"));
  m.def("draw_rectangle_gradient_h", &DrawRectangleGradientH, nb::arg("posX"), nb::arg("posY"), nb::arg("width"), nb::arg("height"), nb::arg("left"), nb::arg("right"));
  m.def("draw_rectangle_gradient_ex", &DrawRectangleGradientEx, nb::arg("rec"), nb::arg("topLeft"), nb::arg("bottomLeft"), nb::arg("bottomRight"), nb::arg("topRight"));
  m.def("draw_rectangle_lines", &DrawRectangleLines, nb::arg("posX"), nb::arg("posY"), nb::arg("width"), nb::arg("height"), nb::arg("color"));
  m.def("draw_rectangle_lines_ex", &DrawRectangleLinesEx, nb::arg("rec"), nb::arg("lineThick"), nb::arg("color"));
  m.def("draw_rectangle_rounded", &DrawRectangleRounded, nb::arg("rec"), nb::arg("roundness"), nb::arg("segments"), nb::arg("color"));
  m.def("draw_rectangle_rounded_lines_ex", &DrawRectangleRoundedLinesEx, nb::arg("rec"), nb::arg("roundness"), nb::arg("segments"), nb::arg("lineThick"), nb::arg("color"));
  m.def("check_collision_recs", &CheckCollisionRecs, nb::arg("rec1"), nb::arg("rec2"));
  m.def("check_collision_point_rec", &CheckCollisionPointRec, nb::arg("point"), nb::arg("rec"));
  m.def("get_collision_rec", &GetCollisionRec, nb::arg("rec1"), nb::arg("rec2"));
  m.def("load_image", &LoadImage, nb::arg("fileName"), nb::call_guard<nb::gil_scoped_release>());
  m.def("load_image_from_texture", &LoadImageFromTexture, nb::arg("texture"), nb::call_guard<nb::gil_scoped_release>());
  m.def("unload_image", [](nb::handle value) {
    if (nb::hasattr(value, "_buffer")) throw nb::value_error("cannot unload an image backed by a Python buffer");
    auto &image = nb::cast<Image &>(value);
    if (image.data) {
      nb::gil_scoped_release release;
      UnloadImage(image);
    }
    image = {};
  }, nb::arg("image"));
  m.def("gen_image_color", &GenImageColor, nb::arg("width"), nb::arg("height"), nb::arg("color"), nb::call_guard<nb::gil_scoped_release>());
  m.def("load_texture_from_image", [](nb::handle value) {
    const Image image = checked_image(value);
    nb::gil_scoped_release release;
    return LoadTextureFromImage(image);
  }, nb::arg("image"));
  m.def("load_render_texture", &LoadRenderTexture, nb::arg("width"), nb::arg("height"), nb::call_guard<nb::gil_scoped_release>());
  m.def("unload_texture", &UnloadTexture, nb::arg("texture"), nb::call_guard<nb::gil_scoped_release>());
  m.def("unload_render_texture", &UnloadRenderTexture, nb::arg("target"), nb::call_guard<nb::gil_scoped_release>());
  m.def("set_texture_filter", &SetTextureFilter, nb::arg("texture"), nb::arg("filter"));
  m.def("set_texture_wrap", &SetTextureWrap, nb::arg("texture"), nb::arg("wrap"));
  m.def("draw_texture", &DrawTexture, nb::arg("texture"), nb::arg("posX"), nb::arg("posY"), nb::arg("tint"));
  m.def("draw_texture_v", &DrawTextureV, nb::arg("texture"), nb::arg("position"), nb::arg("tint"));
  m.def("draw_texture_ex", &DrawTextureEx, nb::arg("texture"), nb::arg("position"), nb::arg("rotation"), nb::arg("scale"), nb::arg("tint"));
  m.def("draw_texture_pro", &DrawTexturePro, nb::arg("texture"), nb::arg("source"), nb::arg("dest"), nb::arg("origin"), nb::arg("rotation"), nb::arg("tint"));
  m.def("color_to_hsv", &ColorToHSV, nb::arg("color"));
  m.def("color_from_hsv", &ColorFromHSV, nb::arg("hue"), nb::arg("saturation"), nb::arg("value"));
  m.def("unload_font", &UnloadFont, nb::arg("font"), nb::call_guard<nb::gil_scoped_release>());
  m.def("draw_fps", &DrawFPS, nb::arg("posX"), nb::arg("posY"));
  m.def("draw_text_ex", &DrawTextEx, nb::arg("font"), nb::arg("text"), nb::arg("position"), nb::arg("fontSize"), nb::arg("spacing"), nb::arg("tint"));
  m.def("measure_text_ex", &MeasureTextEx, nb::arg("font"), nb::arg("text"), nb::arg("fontSize"), nb::arg("spacing"));
  m.def("rl_push_matrix", &rlPushMatrix);
  m.def("rl_pop_matrix", &rlPopMatrix);
  m.def("rl_translatef", &rlTranslatef, nb::arg("x"), nb::arg("y"), nb::arg("z"));
  m.def("rl_scalef", &rlScalef, nb::arg("x"), nb::arg("y"), nb::arg("z"));
  m.def("matrix_ortho", &MatrixOrtho, nb::arg("left"), nb::arg("right"), nb::arg("bottom"), nb::arg("top"), nb::arg("nearPlane"), nb::arg("farPlane"));
  m.def("fade", &Fade, nb::arg("color"), nb::arg("alpha"));
  m.def("get_font_default", &GetFontDefault);
  m.def("image_resize", [](nb::handle value, int width, int height) {
    if (nb::hasattr(value, "_buffer")) throw nb::value_error("cannot resize an image backed by a Python buffer");
    checked_pixel_size(width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    auto &image = nb::cast<Image &>(value);
    nb::gil_scoped_release release;
    ImageResize(&image, width, height);
  }, nb::arg("image"), nb::arg("newWidth"), nb::arg("newHeight"));
  m.def("image_flip_horizontal", [](nb::handle value) {
    if (nb::hasattr(value, "_buffer")) throw nb::value_error("cannot modify an image backed by a Python buffer");
    auto &image = nb::cast<Image &>(value);
    nb::gil_scoped_release release;
    ImageFlipHorizontal(&image);
  }, nb::arg("image"));
  m.def("image_alpha_premultiply", [](nb::handle value) {
    if (nb::hasattr(value, "_buffer")) throw nb::value_error("cannot modify an image backed by a Python buffer");
    auto &image = nb::cast<Image &>(value);
    nb::gil_scoped_release release;
    ImageAlphaPremultiply(&image);
  }, nb::arg("image"));
  m.def("gen_texture_mipmaps", [](Texture &texture) { nb::gil_scoped_release release; GenTextureMipmaps(&texture); }, nb::arg("texture"));
  m.def("update_texture", [](Texture texture, nb::handle pixels) {
    RaylibBuffer buffer(pixels);
    const int required = checked_pixel_size(texture.width, texture.height, texture.format);
    if (required <= 0 || buffer.view.len < required) throw nb::value_error("texture pixel buffer is too small or texture dimensions are invalid");
    nb::gil_scoped_release release;
    UpdateTexture(texture, buffer.view.buf);
  }, nb::arg("texture"), nb::arg("pixels"));
  m.def("load_font_ex", [](const char *file, int size, nb::object codepoints, int count) {
    if (count < 0) throw nb::value_error("codepoint count must be nonnegative");
    if (codepoints.is_none()) {
      nb::gil_scoped_release release;
      return LoadFontEx(file, size, nullptr, count);
    }
    auto points = nb::cast<std::vector<int>>(codepoints);
    if (size_t(count) > points.size()) throw nb::value_error("codepoint count exceeds the supplied codepoints");
    nb::gil_scoped_release release;
    return LoadFontEx(file, size, points.data(), count);
  }, nb::arg("fileName"), nb::arg("fontSize"), nb::arg("codepoints").none(), nb::arg("codepointCount"));
  m.def("load_shader_from_memory", [](nb::object vertex, nb::object fragment) {
    const std::string vs = vertex.is_none() ? "" : nb::cast<std::string>(vertex);
    const std::string fs = fragment.is_none() ? "" : nb::cast<std::string>(fragment);
    const char *vertex_source = vertex.is_none() ? nullptr : vs.c_str();
    const char *fragment_source = fragment.is_none() ? nullptr : fs.c_str();
    nb::gil_scoped_release release;
    return LoadShaderFromMemory(vertex_source, fragment_source);
  }, nb::arg("vsCode").none(), nb::arg("fsCode").none());
  m.def("set_shader_value", [](Shader shader, int location, nb::handle value, int type) {
    shader_value(shader, location, value, type, 1);
  }, nb::arg("shader"), nb::arg("locIndex"), nb::arg("value"), nb::arg("uniformType"));
  m.def("set_shader_value_v", &shader_value, nb::arg("shader"), nb::arg("locIndex"), nb::arg("value"), nb::arg("uniformType"), nb::arg("count"));
  m.def("draw_triangle_fan", [](nb::sequence values, int count, Color color) {
    auto points = raylib_points(values);
    check_point_count(points.size(), count);
    DrawTriangleFan(points.data(), count, color);
  }, nb::arg("points"), nb::arg("pointCount"), nb::arg("color"));
  m.def("draw_triangle_strip", [](nb::sequence values, int count, Color color) {
    auto points = raylib_points(values);
    check_point_count(points.size(), count);
    DrawTriangleStrip(points.data(), count, color);
  }, nb::arg("points"), nb::arg("pointCount"), nb::arg("color"));
  m.def("draw_spline_linear", [](nb::sequence values, int count, float thick, Color color) {
    auto points = raylib_points(values);
    check_point_count(points.size(), count);
    DrawSplineLinear(points.data(), count, thick, color);
  }, nb::arg("points"), nb::arg("pointCount"), nb::arg("thick"), nb::arg("color"));
  m.def("set_trace_log_callback", [](nb::object callback) {
    if (!callback.is_none() && !PyCallable_Check(callback.ptr())) throw nb::type_error("trace callback must be callable or None");
    PyObject *old = raylib_trace_callback;
    raylib_trace_callback = callback.is_none() ? nullptr : callback.inc_ref().ptr();
    Py_XDECREF(old);
    SetTraceLogCallback(raylib_trace_callback ? raylib_trace : nullptr);
  }, nb::arg("callback").none());
  m.def("trace_log", [](int level, const char *message) { TraceLog(level, "%s", message); }, nb::arg("logLevel"), nb::arg("message"));
}
