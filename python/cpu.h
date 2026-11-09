#pragma once

// raylib_nb.cpu: the software rasterizer of rcpu.c
#include <rcpu.h>

// the target's buffer export, held until the next target
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
  nb::module_ m = root.def_submodule("cpu", "software rasterizer, and on comma devices the panel it scans out to");
  using release = nb::call_guard<nb::gil_scoped_release>;

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
  m.def("clip", &cr_clip, "x0"_a, "y0"_a, "x1"_a, "y1"_a);
  m.def("clear", [](Color c) { cr_clear(cpu_color(c)); }, "color"_a, release());
  m.def("rect", [](float x0, float y0, float x1, float y1, Color tl, Color tr, Color bl, Color br) {
    cr_rect(x0, y0, x1, y1, cpu_color(tl), cpu_color(tr), cpu_color(bl), cpu_color(br));
  }, "x0"_a, "y0"_a, "x1"_a, "y1"_a, "tl"_a, "tr"_a, "bl"_a, "br"_a, release());
  m.def("rounded_rect", [](Rectangle r, float radius, float thickness, Color c) {
    cr_rounded_rect(r.x, r.y, r.width, r.height, radius, thickness, cpu_color(c));
  }, "rec"_a, "radius"_a, "thickness"_a, "color"_a, release());
  m.def("ring", [](Vector2 center, float r0, float r1, float a0, float a1, Color inner, Color outer) {
    cr_ring(center.x, center.y, r0, r1, a0, a1, cpu_color(inner), cpu_color(outer));
  }, "center"_a, "r0"_a, "r1"_a, "a0"_a, "a1"_a, "inner"_a, "outer"_a, release());
  // points are float32 x, y pairs and colors uint32 packed colors, one per point or a single one
  m.def("triangles", [](nb::handle points, nb::handle colors, int mode) {
    RaylibBuffer p(points), c(colors);
    if (!p.view.format || std::strcmp(p.view.format, "f") || c.view.itemsize != 4) throw nb::type_error("points must be float32 and colors uint32");
    const int n = int(p.view.len / 8), nc = int(c.view.len / 4);
    if (nc != 1 && nc != n) throw nb::value_error("give one color, or one per point");
    nb::gil_scoped_release gil;
    cr_triangles(static_cast<const float *>(p.view.buf), n, static_cast<const uint32_t *>(c.view.buf), nc, mode);
  }, "points"_a, "colors"_a, "mode"_a);
  // raylib's DrawTexturePro from premultiplied RGBA pixels, or an alpha mask when a8
  m.def("blit", [](nb::handle pixels, int width, int height, int stride, bool a8, Rectangle src, Rectangle dst, Vector2 origin,
                   float rotation, Color tint, bool bilinear) {
    RaylibBuffer b(pixels);
    if (width < 0 || height < 0 || stride < width * (a8 ? 1 : 4)) throw nb::value_error("invalid texture dimensions");
    cpu_check(b, size_t(stride) * size_t(height), "texture buffer is smaller than its dimensions");
    nb::gil_scoped_release gil;
    cr_blit(static_cast<const uint8_t *>(b.view.buf), width, height, stride, a8, src.x, src.y, src.width, src.height,
            dst.x, dst.y, dst.width, dst.height, origin.x, origin.y, rotation, cpu_color(tint), bilinear);
  }, "pixels"_a, "width"_a, "height"_a, "stride"_a, "a8"_a, "src"_a, "dst"_a, "origin"_a, "rotation"_a, "tint"_a, "bilinear"_a);
  // an NV12 frame, opaque. curve maps each channel from -512 to 767, saturation is out of 256
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

  // the glyphs raylib renders of a font file at size: (codepoint, advance, offset x, offset y, alpha bytes, width, height)
  m.def("glyphs", [](nb::bytes font, int size, std::vector<int> codepoints) {
    int count = 0;
    GlyphInfo *glyphs;
    {
      nb::gil_scoped_release gil;
      glyphs = LoadFontData(reinterpret_cast<const unsigned char *>(font.c_str()), int(font.size()), size, codepoints.data(),
                            int(codepoints.size()), FONT_DEFAULT, &count);
    }
    nb::list out;
    for (int i = 0; glyphs && i < count; i++) {
      const Image &image = glyphs[i].image;
      const size_t n = image.data ? size_t(image.width) * size_t(image.height) : 0;
      out.append(nb::make_tuple(glyphs[i].value, glyphs[i].advanceX, glyphs[i].offsetX, glyphs[i].offsetY,
                                nb::bytes(static_cast<const char *>(image.data), n), n ? image.width : 0, n ? image.height : 0));
    }
    UnloadFontData(glyphs, count);
    return out;
  }, "font"_a, "size"_a, "codepoints"_a);

  m.def("display_init", &cr_display_init, "fd"_a, "canonical_zero"_a, release());
  // the panel buffer to draw into, valid until display_close
  m.def("display_target", [](int width, int height) -> nb::object {
    uint8_t *map = cr_display_target(width, height);
    if (!map) return nb::none();
    return nb::steal(PyMemoryView_FromMemory(reinterpret_cast<char *>(map), Py_ssize_t(width) * height * 4, PyBUF_WRITE));
  }, "width"_a, "height"_a);
  m.def("display_blob", [](const std::string &name, nb::handle data) {
    if (data.is_none()) return cr_display_blob(name.c_str(), nullptr, 0);
    RaylibBuffer b(data);
    return cr_display_blob(name.c_str(), b.view.buf, uint32_t(b.view.len));
  }, "name"_a, "data"_a.none());
  m.def("display_present", &cr_display_present, release());
  m.def("display_close", [] {
    cpu_release_target();
    cr_target(nullptr, 0, 0, 0);
    cr_display_close();
  });
}
