// rcpu: software rasterizer for the openpilot UI on comma devices, see rcpu.c
#ifndef RCPU_H
#define RCPU_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// drawing into a premultiplied RGBA8 target. colors are packed r | g << 8 | b << 16 | a << 24, straight alpha
void cr_target(uint8_t *pixels, int width, int height, int stride);
void cr_clip(int x0, int y0, int x1, int y1);
void cr_clear(uint32_t color);
void cr_rect(float x0, float y0, float x1, float y1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
void cr_rounded_rect(float x, float y, float w, float h, float r, float thickness, uint32_t color);
void cr_ring(float cx, float cy, float r0, float r1, float a0, float a1, uint32_t inner, uint32_t outer);
void cr_triangles(const float *xy, int n, const uint32_t *colors, int ncolors, int mode);
void cr_blit(const uint8_t *src, int sw, int sh, int sstride, int a8, float sx, float sy, float src_w, float src_h,
             float dx, float dy, float dw, float dh, float ox, float oy, float rotation, uint32_t tint, int bilinear);
void cr_nv12(const uint8_t *y_plane, const uint8_t *uv_plane, int stride, int fw, int fh, float dx, float dy, float dw, float dh,
             int flip_x, const uint8_t *curve, int saturation, int new_frame);

// the panel, on PLATFORM_COMMA. errors are negative errno
int cr_display_init(int fd, int canonical_zero);
uint8_t *cr_display_target(int width, int height);
int cr_display_blob(const char *name, const void *data, uint32_t size);
int cr_display_present(void);
void cr_display_close(void);

#if defined(__cplusplus)
}
#endif

#endif
