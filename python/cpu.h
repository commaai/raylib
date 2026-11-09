#pragma once

// raylib_nb.cpu: rcpu, the software rasterizer that draws for raylib's software renderer
#include <cerrno>
#include <cstring>

#include <rcpu.h>

// the export of the target's buffer, held until the next target
inline Py_buffer *cpu_target_view = nullptr;

inline void cpu_release_target() {
  if (cpu_target_view) {
    PyBuffer_Release(cpu_target_view);
    delete cpu_target_view;
    cpu_target_view = nullptr;
  }
}

inline uint32_t cpu_color(Color c) { return c.r | c.g << 8 | c.b << 16 | uint32_t(c.a) << 24; }

inline void cpu_check(const RaylibBuffer &buffer, size_t size, const char *what) {
  if (size_t(buffer.view.len) < size) throw nb::value_error(what);
}

inline void bind_cpu(nb::module_ &root) {
  nb::module_ m = root.def_submodule("cpu", "rcpu, the software rasterizer that draws for raylib's software renderer");
  // whether raylib draws on the CPU, and so rcpu can draw onto its screen
  m.attr("software") = rlGetVersion() == RL_OPENGL_SOFTWARE;
  m.attr("TRIANGLE_LIST") = int(CR_TRIANGLE_LIST);
  m.attr("TRIANGLE_STRIP") = int(CR_TRIANGLE_STRIP);
  m.attr("TRIANGLE_FAN") = int(CR_TRIANGLE_FAN);

  // draw into a buffer of premultiplied RGBA8 pixels instead, for tests: until raylib draws, which targets its own again
  m.def("target", [](nb::handle pixels, int width, int height, int stride) {
    auto *view = new Py_buffer{};
    if (PyObject_GetBuffer(pixels.ptr(), view, PyBUF_CONTIG) != 0) {
      delete view;
      throw nb::python_error();
    }
    if (width <= 0 || height <= 0 || stride < width * 4 || size_t(view->len) < size_t(stride) * size_t(height)) {
      PyBuffer_Release(view);
      delete view;
      throw nb::value_error("target buffer is smaller than its dimensions");
    }
    cpu_release_target();
    cpu_target_view = view;
    cr_target(static_cast<uint8_t *>(view->buf), width, height, stride);
  }, "pixels"_a, "width"_a, "height"_a, "stride"_a);

  // draw where raylib draws, after what it drew so far, at its 2D coordinates and in its scissor
  m.def("target_raylib", []() {
    int ret = cr_target_raylib();
    if (ret) throw nb::builtin_exception(nb::exception_type::runtime_error, std::strerror(-ret));
    cpu_release_target();
  });

  // rect, triangles and blit are what raylib's own drawing does with rcpu, for tests
  m.def("rect", [](float x0, float y0, float x1, float y1, Color top_left, Color top_right, Color bottom_left, Color bottom_right) {
    cr_rect(x0, y0, x1, y1, cpu_color(top_left), cpu_color(top_right), cpu_color(bottom_left), cpu_color(bottom_right));
  }, "x0"_a, "y0"_a, "x1"_a, "y1"_a, "top_left"_a, "top_right"_a, "bottom_left"_a, "bottom_right"_a,
     nb::call_guard<nb::gil_scoped_release>());

  // points are float32 x, y pairs, and colors packed uint32 colors, one per point or one for all
  m.def("triangles", [](nb::handle points, nb::handle colors, int mode) {
    RaylibBuffer p(points), c(colors);
    if (!p.view.format || std::strcmp(p.view.format, "f") || c.view.itemsize != 4) throw nb::type_error("points must be float32 and colors uint32");
    const int count = int(p.view.len / 8), color_count = int(c.view.len / 4);
    if (color_count != 1 && color_count != count) throw nb::value_error("give one color, or one per point");
    if (mode < CR_TRIANGLE_LIST || mode > CR_TRIANGLE_FAN) throw nb::value_error("invalid triangle mode");
    nb::gil_scoped_release gil;
    cr_triangles(static_cast<const float *>(p.view.buf), count, static_cast<const uint32_t *>(c.view.buf), color_count, mode);
  }, "points"_a, "colors"_a, "mode"_a);

  // points are float32 x, y pairs, and colors packed uint32 straight alpha colors: a ramp from t = 0 to 1 at
  // t = tx * x + ty * y + t0, or one color
  m.def("polygon", [](nb::handle points, nb::handle colors, float tx, float ty, float t0) {
    RaylibBuffer p(points), c(colors);
    if (!p.view.format || std::strcmp(p.view.format, "f") || c.view.itemsize != 4) throw nb::type_error("points must be float32 and colors uint32");
    const int count = int(p.view.len / 8), color_count = int(c.view.len / 4);
    if (color_count < 1 || color_count > 1024) throw nb::value_error("give 1 to 1024 colors");
    nb::gil_scoped_release gil;
    cr_polygon(static_cast<const float *>(p.view.buf), count, static_cast<const uint32_t *>(c.view.buf), color_count, tx, ty, t0);
  }, "points"_a, "colors"_a, "tx"_a = 0.0f, "ty"_a = 0.0f, "t0"_a = 0.0f);

  // raylib's DrawTexturePro from premultiplied RGBA8 pixels, or an alpha mask when a8
  m.def("blit", [](nb::handle pixels, int width, int height, int stride, bool a8, Rectangle src, Rectangle dst, Vector2 origin,
                   float rotation, Color tint, bool bilinear) {
    RaylibBuffer b(pixels);
    if (width < 0 || height < 0 || stride < width * (a8 ? 1 : 4)) throw nb::value_error("invalid texture dimensions");
    cpu_check(b, size_t(stride) * size_t(height), "texture buffer is smaller than its dimensions");
    nb::gil_scoped_release gil;
    cr_blit(static_cast<const uint8_t *>(b.view.buf), width, height, stride, a8, src.x, src.y, src.width, src.height,
            dst.x, dst.y, dst.width, dst.height, origin.x, origin.y, rotation, cpu_color(tint), bilinear);
  }, "pixels"_a, "width"_a, "height"_a, "stride"_a, "a8"_a, "src"_a, "dst"_a, "origin"_a, "rotation"_a, "tint"_a, "bilinear"_a);

  // an NV12 frame, opaque. curve maps each channel from -512 to 767, and saturation is out of 256
  m.def("nv12", [](nb::handle frame, int uv_offset, int stride, int width, int height, Rectangle dst, bool flip_x,
                   nb::handle curve, int saturation, bool new_frame) {
    RaylibBuffer f(frame), c(curve);
    if (width <= 0 || height <= 0 || stride < width || uv_offset < stride * height) throw nb::value_error("invalid frame dimensions");
    cpu_check(f, size_t(uv_offset) + size_t(stride) * size_t((height + 1) / 2), "frame buffer is smaller than its dimensions");
    cpu_check(c, 1280, "curve needs 1280 entries");
    const auto *data = static_cast<const uint8_t *>(f.view.buf);
    nb::gil_scoped_release gil;
    cr_nv12(data, data + uv_offset, stride, width, height, dst.x, dst.y, dst.width, dst.height, flip_x,
            static_cast<const uint8_t *>(c.view.buf), saturation, new_frame);
  }, "frame"_a, "uv_offset"_a, "stride"_a, "width"_a, "height"_a, "dst"_a, "flip_x"_a, "curve"_a, "saturation"_a, "new_frame"_a);
}
