#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <raylib.h>
#include <cstring>

namespace nb = nanobind;
using namespace nb::literals;

#include "functions.h"

#ifndef RAYLIB_MODULE
#define RAYLIB_MODULE _raylib_desktop
#endif

NB_MODULE(RAYLIB_MODULE, m) {
  nb::class_<Vector2>(m, "Vector2")
    .def(nb::init<float, float>(), "x"_a=0.f, "y"_a=0.f)
    .def_rw("x", &Vector2::x).def_rw("y", &Vector2::y);
  nb::class_<Vector3>(m, "Vector3")
    .def(nb::init<float, float, float>(), "x"_a=0.f, "y"_a=0.f, "z"_a=0.f)
    .def_rw("x", &Vector3::x).def_rw("y", &Vector3::y).def_rw("z", &Vector3::z);
  nb::class_<Vector4>(m, "Vector4")
    .def(nb::init<float, float, float, float>(), "x"_a=0.f, "y"_a=0.f, "z"_a=0.f, "w"_a=0.f)
    .def_rw("x", &Vector4::x).def_rw("y", &Vector4::y).def_rw("z", &Vector4::z).def_rw("w", &Vector4::w);
  nb::class_<Rectangle>(m, "Rectangle")
    .def(nb::init<float, float, float, float>(), "x"_a=0.f, "y"_a=0.f, "width"_a=0.f, "height"_a=0.f)
    .def_rw("x", &Rectangle::x).def_rw("y", &Rectangle::y)
    .def_rw("width", &Rectangle::width).def_rw("height", &Rectangle::height);
  nb::class_<Color>(m, "Color")
    .def(nb::init<unsigned char, unsigned char, unsigned char, unsigned char>(), "r"_a=0, "g"_a=0, "b"_a=0, "a"_a=0)
    .def_rw("r", &Color::r).def_rw("g", &Color::g).def_rw("b", &Color::b).def_rw("a", &Color::a);
  nb::class_<Texture>(m, "Texture")
    .def(nb::init<unsigned int, int, int, int, int>(), "id"_a=0, "width"_a=0, "height"_a=0, "mipmaps"_a=0, "format"_a=0)
    .def_rw("id", &Texture::id).def_rw("width", &Texture::width).def_rw("height", &Texture::height)
    .def_rw("mipmaps", &Texture::mipmaps).def_rw("format", &Texture::format);
  nb::class_<Image>(m, "Image", nb::dynamic_attr())
    .def("__init__", [](Image *self, nb::object data, int width, int height, int mipmaps, int format) {
      void *ptr = nullptr;
      if (!data.is_none()) {
        if (mipmaps != 1) throw nb::value_error("Python-backed images must have exactly one mipmap level");
        // Retain the buffer export, which also prevents resize while Image points at it.
        auto *view = new Py_buffer{};
        if (PyObject_GetBuffer(data.ptr(), view, PyBUF_CONTIG_RO) < 0) {
          delete view;
          throw nb::python_error();
        }
        int expected;
        try { expected = checked_pixel_size(width, height, format); }
        catch (...) { PyBuffer_Release(view); delete view; throw; }
        if (expected < 0 || view->len < expected) {
          PyBuffer_Release(view);
          delete view;
          throw nb::value_error("image buffer is smaller than its dimensions and pixel format");
        }
        ptr = view->buf;
        nb::find(self).attr("_buffer") = nb::capsule(view, [](void *p) noexcept {
          auto *v = static_cast<Py_buffer *>(p);
          PyBuffer_Release(v);
          delete v;
        });
      }
      new (self) Image{ptr, width, height, mipmaps, format};
    }, "data"_a=nb::none(), "width"_a=0, "height"_a=0, "mipmaps"_a=0, "format"_a=0)
    .def_rw("width", &Image::width).def_rw("height", &Image::height)
    .def_rw("mipmaps", &Image::mipmaps).def_rw("format", &Image::format)
    .def("to_bytes", [](nb::handle value, size_t size) {
      const auto &image = checked_image(value);
      if (!image.data && size) throw nb::value_error("image has no pixel data");
      const int expected = checked_pixel_size(image.width, image.height, image.format);
      if (expected < 0 || size > static_cast<size_t>(expected)) throw nb::value_error("requested byte count exceeds image size");
      return nb::bytes(static_cast<const char *>(image.data), size);
    }, "size"_a);
  nb::class_<RenderTexture>(m, "RenderTexture")
    .def(nb::init<unsigned int, Texture, Texture>(), "id"_a=0, "texture"_a=Texture{}, "depth"_a=Texture{})
    .def_rw("id", &RenderTexture::id).def_rw("texture", &RenderTexture::texture).def_rw("depth", &RenderTexture::depth);
  nb::class_<GlyphInfo>(m, "GlyphInfo")
    .def(nb::init<>()).def_rw("value", &GlyphInfo::value).def_rw("offsetX", &GlyphInfo::offsetX)
    .def_rw("offsetY", &GlyphInfo::offsetY).def_rw("advanceX", &GlyphInfo::advanceX).def_rw("image", &GlyphInfo::image);
  nb::class_<Font>(m, "Font")
    .def(nb::init<>()).def_rw("baseSize", &Font::baseSize).def_rw("glyphCount", &Font::glyphCount)
    .def_rw("glyphPadding", &Font::glyphPadding).def_rw("texture", &Font::texture);
  nb::class_<Shader>(m, "Shader")
    .def(nb::init<>()).def_rw("id", &Shader::id);
  nb::class_<Matrix>(m, "Matrix").def(nb::init<>())
    .def_rw("m0", &Matrix::m0).def_rw("m1", &Matrix::m1).def_rw("m2", &Matrix::m2).def_rw("m3", &Matrix::m3)
    .def_rw("m4", &Matrix::m4).def_rw("m5", &Matrix::m5).def_rw("m6", &Matrix::m6).def_rw("m7", &Matrix::m7)
    .def_rw("m8", &Matrix::m8).def_rw("m9", &Matrix::m9).def_rw("m10", &Matrix::m10).def_rw("m11", &Matrix::m11)
    .def_rw("m12", &Matrix::m12).def_rw("m13", &Matrix::m13).def_rw("m14", &Matrix::m14).def_rw("m15", &Matrix::m15);
  m.attr("Texture2D") = m.attr("Texture");
  m.attr("RenderTexture2D") = m.attr("RenderTexture");
  m.attr("RAYLIB_VERSION_MAJOR") = RAYLIB_VERSION_MAJOR;
  bind_functions(m);
  // Release the native-held Python reference before interpreter teardown. A
  // module capsule alone cannot break a callback -> globals -> module cycle.
  m.def("_clear_trace_log_callback", &clear_trace_log_callback);
  nb::module_::import_("atexit").attr("register")(m.attr("_clear_trace_log_callback"));
  m.attr("_callback_cleanup") = nb::capsule(&raylib_trace_callback, [](void *) noexcept {
    if (Py_IsInitialized()) clear_trace_log_callback();
  });
}
