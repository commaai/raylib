// rcpu: a software rasterizer for the 2D drawing of raylib's software renderer, see rcpu.c
#ifndef RCPU_H
#define RCPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// draw into premultiplied RGBA8 pixels, with rows stride bytes apart. a negative stride is bottom up
void cr_target(uint8_t *pixels, int width, int height, int stride);
// draw only into the pixels x0 <= x < x1, y0 <= y < y1
void cr_clip(int x0, int y0, int x1, int y1);
// draw at coordinates scaled by scale and then translated by (tx, ty)
void cr_transform(float scale, float tx, float ty);
// draw where raylib's software renderer draws, its screen or a render texture, after what it drew so far, at its 2D
// coordinates and in its scissor. 0, or a negative errno when raylib draws with the GPU (ENODEV), into a texture that
// isn't RGBA8 (ENOTSUP), or rotated (EINVAL). rcpu blends like GL's alpha blending where what's drawn over is opaque
int cr_target_raylib(void);
// the pixels of raylib's screen, as drawn so far: premultiplied RGBA8 rows, bottom up like GL's. 0, or -ENODEV when raylib
// draws with the GPU
int cr_raylib_screen(const uint8_t **pixels, int *width, int *height);

// colors are packed r | g << 8 | b << 16 | a << 24, straight alpha
void cr_premultiply(uint32_t *dst, const uint32_t *src, size_t count);

// a rectangle with a color at each corner, interpolated across it
void cr_rect(float x0, float y0, float x1, float y1, uint32_t top_left, uint32_t top_right, uint32_t bottom_left,
             uint32_t bottom_right);

// triangles of count points, x and y each, with a color per point or one color
enum { CR_TRIANGLE_LIST, CR_TRIANGLE_STRIP, CR_TRIANGLE_FAN };
void cr_triangles(const float *xy, int count, const uint32_t *colors, int color_count, int mode);

// a polygon of count points, x and y each, drawn where it winds around a pixel's center. a pixel at (x, y) is the color
// at t = tx * x + ty * y + t0, clamped to [0, 1], along a ramp of color_count colors from t = 0 to 1. one color fills it
void cr_polygon(const float *xy, int count, const uint32_t *colors, int color_count, float tx, float ty, float t0);

// like raylib's DrawTexturePro: the source rectangle (sx, sy, sw, sh) drawn into the destination (dx, dy, dw, dh), rotated by
// rotation degrees around the origin (ox, oy) from the destination's top left. a negative source size flips.
// src is premultiplied RGBA8, or an alpha mask when a8
void cr_blit(const uint8_t *src, int src_width, int src_height, int src_stride, bool a8, float sx, float sy, float sw,
             float sh, float dx, float dy, float dw, float dh, float ox, float oy, float rotation, uint32_t tint, bool bilinear);

// an NV12 frame drawn opaque into the destination (dx, dy, dw, dh), sampled at nearest and converted like the GPU's NV12
// import (limited range BT.601), then mixed with its luma by saturation/256 and each channel mapped through curve, as
// openpilot's frame shaders do. like on the GPU, channels can be outside [0, 255] until the curve, which has 1280 entries for
// -512 to 767. a frame drawn again, until new_frame, is copied from its last conversion
void cr_nv12(const uint8_t *y_plane, const uint8_t *uv_plane, int stride, int width, int height, float dx, float dy, float dw,
             float dh, bool flip_x, const uint8_t *curve, int saturation, bool new_frame);

#if defined(__cplusplus)
}
#endif

#endif
