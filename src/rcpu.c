// rcpu: a software rasterizer for the 2D drawing of raylib's software renderer, rlsw, which it does several times faster.
// rcpu_rlsw.c hands it what rlsw draws, and it draws what has no fixed function equivalent, like a GPU's frame shaders.
//
// Pixels are premultiplied RGBA8, which is DRM's ABGR8888. Colors are packed r | g << 8 | b << 16 | a << 24, straight alpha.
// Pixel centers are at halves, and a pixel is drawn when its center is inside a shape.
#include "rcpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

static struct {
  uint8_t *pixels;
  int width, height, stride;
  int x0, y0, x1, y1;  // the clip
  float scale, tx, ty;  // the transform
} T;

void cr_target(uint8_t *pixels, int width, int height, int stride) {
  T.pixels = pixels;
  T.width = width;
  T.height = height;
  T.stride = stride;
  cr_clip(0, 0, width, height);
  cr_transform(1.0f, 0.0f, 0.0f);
}

void cr_clip(int x0, int y0, int x1, int y1) {
  T.x0 = CLAMP(x0, 0, T.width);
  T.y0 = CLAMP(y0, 0, T.height);
  T.x1 = CLAMP(x1, T.x0, T.width);
  T.y1 = CLAMP(y1, T.y0, T.height);
}

void cr_transform(float scale, float tx, float ty) {
  T.scale = scale;
  T.tx = tx;
  T.ty = ty;
}

static inline float to_x(float x) {
  return x * T.scale + T.tx;
}

static inline float to_y(float y) {
  return y * T.scale + T.ty;
}

static inline uint32_t *row(int y) {
  return (uint32_t *)(T.pixels + (ptrdiff_t)y * T.stride);
}

// have n pixels from p cached before they're drawn. a screen's rows are far apart and mostly out of the cache, which
// stalls drawing narrow things, like glyphs, row after row
static inline void prefetch(const uint32_t *p, int n) {
#if defined(__GNUC__) || defined(__clang__)
  for (int i = 0; i < n; i += 16) __builtin_prefetch(p + i, 1);  // 16 pixels to a 64 byte cache line
  __builtin_prefetch(p + n - 1, 1);
#endif
}

// the first pixel whose center is at or after a
static inline int first_pixel(float a) {
  return (int)ceilf(a - 0.5f);
}

// ***** colors *****

static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return r | g << 8 | b << 16 | a << 24;
}

static inline uint32_t alpha(uint32_t c) {
  return c >> 24;
}

// all four channels scaled by a/255, rounded. two channels at a time: red with blue, and green with alpha.
// x/255 is computed as (x + x/256) / 256
static inline uint32_t mul(uint32_t c, uint32_t a) {
  uint32_t red_blue = (c & 0x00ff00ff) * a + 0x00800080;
  uint32_t green_alpha = ((c >> 8) & 0x00ff00ff) * a + 0x00800080;
  red_blue = ((red_blue + ((red_blue >> 8) & 0x00ff00ff)) >> 8) & 0x00ff00ff;
  green_alpha = (green_alpha + ((green_alpha >> 8) & 0x00ff00ff)) & 0xff00ff00;
  return red_blue | green_alpha;
}

// each channel of a times the same channel of b, out of 255
static inline uint32_t mul4(uint32_t a, uint32_t b) {
  uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    uint32_t product = ((a >> shift) & 255) * ((b >> shift) & 255) + 128;
    uint32_t channel = (product + (product >> 8)) >> 8;
    result |= channel << shift;
  }
  return result;
}

// straight alpha to premultiplied: the color scaled by alpha, alpha kept
static inline uint32_t premultiply(uint32_t c) {
  uint32_t a = alpha(c);
  uint32_t color = mul(c, a) & 0x00ffffff;
  return color | a << 24;
}

// premultiplied src drawn over dst
static inline uint32_t over(uint32_t dst, uint32_t src) {
  uint32_t uncovered = 255 - alpha(src);
  return src + mul(dst, uncovered);
}

// from a to b by t out of 255
static inline uint32_t lerp(uint32_t a, uint32_t b, uint32_t t) {
  return mul(a, 255 - t) + mul(b, t);
}

void cr_premultiply(uint32_t *dst, const uint32_t *src, size_t count) {
  for (size_t i = 0; i < count; i++) dst[i] = premultiply(src[i]);
}

// n pixels of one premultiplied color
static void span(uint32_t *dst, int n, uint32_t c) {
  if (alpha(c) == 255) {
    for (int i = 0; i < n; i++) dst[i] = c;
  } else if (c != 0) {
    for (int i = 0; i < n; i++) dst[i] = over(dst[i], c);
  }
}

// ***** shapes *****

// the fraction of the way from a to b at t, out of 255
static inline uint32_t fraction(float t, float a, float b) {
  float f = (t - a) / (b - a) * 255.0f + 0.5f;
  return (uint32_t)CLAMP(f, 0.0f, 255.0f);
}

// like GL, colors are interpolated straight and then premultiplied
void cr_rect(float x0, float y0, float x1, float y1, uint32_t top_left, uint32_t top_right, uint32_t bottom_left,
             uint32_t bottom_right) {
  x0 = to_x(x0);
  y0 = to_y(y0);
  x1 = to_x(x1);
  y1 = to_y(y1);
  int left = MAX(first_pixel(x0), T.x0);
  int right = MIN(first_pixel(x1), T.x1);
  int top = MAX(first_pixel(y0), T.y0);
  int bottom = MIN(first_pixel(y1), T.y1);
  if (left >= right || top >= bottom) return;

  // the fraction of the way across at each column, the same for every row
  uint32_t across[right - left];
  if (top_left != top_right || bottom_left != bottom_right) {
    for (int x = left; x < right; x++) across[x - left] = fraction(x + 0.5f, x0, x1);
  }
  for (int y = top; y < bottom; y++) {
    uint32_t down = fraction(y + 0.5f, y0, y1);
    uint32_t row_left = lerp(top_left, bottom_left, down);
    uint32_t row_right = lerp(top_right, bottom_right, down);
    uint32_t *dst = row(y) + left;
    if (row_left == row_right) {
      span(dst, right - left, premultiply(row_left));
      continue;
    }
    for (int i = 0; i < right - left; i++) dst[i] = over(dst[i], premultiply(lerp(row_left, row_right, across[i])));
  }
}

typedef struct {
  float x, y;
} point;

// ***** polygons *****

// a polygon is drawn row by row, between the crossings of its edges with the row's pixel centers where it winds around
// them. an edge's crossings are found from its top end, so polygons sharing an edge, like a fan's triangles, split its
// pixels exactly

typedef struct {
  float x, y, dx_dy;  // its top end, and its slope
  int first_row, end_row;  // the rows whose centers it crosses
  int winding;  // 1 going down, -1 going up
} edge;

typedef struct {
  float x;
  int winding;
} crossing;

// pixels x0 <= x < x1 of row y, drawn with paint
typedef void (*fill_span)(uint32_t *dst, int x0, int x1, int y, const void *paint);

static int compare_first_rows(const void *a, const void *b) {
  return ((const edge *)a)->first_row - ((const edge *)b)->first_row;
}

static void sort_by_first_row(edge *edges, int count) {
  if (count > 16) {
    qsort(edges, (size_t)count, sizeof(edge), compare_first_rows);
    return;
  }
  for (int i = 1; i < count; i++) {
    edge e = edges[i];
    int j = i;
    for (; j > 0 && edges[j - 1].first_row > e.first_row; j--) edges[j] = edges[j - 1];
    edges[j] = e;
  }
}

static void fill_polygon(const point *points, int count, fill_span fill, const void *paint) {
  edge edges[count];
  int edge_count = 0;
  int top = T.y1;
  int bottom = T.y0;
  for (int i = 0; i < count; i++) {
    point p = points[i];
    point q = points[(i + 1) % count];
    if (p.y == q.y) continue;
    bool down = q.y > p.y;
    point top_end = down ? p : q;
    point bottom_end = down ? q : p;
    edge e = {
      .x = top_end.x,
      .y = top_end.y,
      .dx_dy = (bottom_end.x - top_end.x) / (bottom_end.y - top_end.y),
      .first_row = MAX(first_pixel(top_end.y), T.y0),
      .end_row = MIN(first_pixel(bottom_end.y), T.y1),
      .winding = down ? 1 : -1,
    };
    if (e.first_row >= e.end_row) continue;
    top = MIN(top, e.first_row);
    bottom = MAX(bottom, e.end_row);
    edges[edge_count++] = e;
  }
  if (edge_count == 0) return;
  sort_by_first_row(edges, edge_count);

  // row by row, the edges crossing it, sorted along it
  int active[edge_count];
  int active_count = 0;
  int next_edge = 0;
  crossing crossings[edge_count];
  for (int y = top; y < bottom; y++) {
    int still_active = 0;
    for (int i = 0; i < active_count; i++) {
      if (edges[active[i]].end_row > y) active[still_active++] = active[i];
    }
    active_count = still_active;
    while (next_edge < edge_count && edges[next_edge].first_row == y) active[active_count++] = next_edge++;

    float center_y = y + 0.5f;
    for (int i = 0; i < active_count; i++) {
      const edge *e = &edges[active[i]];
      crossing c = {e->x + (center_y - e->y) * e->dx_dy, e->winding};
      int j = i;
      for (; j > 0 && crossings[j - 1].x > c.x; j--) crossings[j] = crossings[j - 1];
      crossings[j] = c;
    }
    uint32_t *dst = row(y);
    int winding = 0;
    for (int i = 0; i + 1 < active_count; i++) {
      winding += crossings[i].winding;
      int x0 = MAX(first_pixel(crossings[i].x), T.x0);
      int x1 = MIN(first_pixel(crossings[i + 1].x), T.x1);
      if (winding != 0 && x0 < x1) fill(dst, x0, x1, y, paint);
    }
  }
}

// a ramp of premultiplied colors along t = t_x * x + t_y * y + t0 at the screen's pixel centers, from t = 0 to 1
typedef struct {
  const uint32_t *colors;
  int count;
  float t_x, t_y, t0;
} ramp;

static void fill_ramp(uint32_t *dst, int x0, int x1, int y, const void *paint) {
  const ramp *r = paint;
  float t_row = r->t0 + r->t_y * (y + 0.5f);
  if (r->count == 1 || r->t_x == 0) {
    float t = CLAMP(t_row + r->t_x * (x0 + 0.5f), 0.0f, 1.0f);
    span(dst + x0, x1 - x0, r->colors[(int)(t * (r->count - 1) + 0.5f)]);
    return;
  }
  for (int x = x0; x < x1; x++) {
    float t = CLAMP(t_row + r->t_x * (x + 0.5f), 0.0f, 1.0f);
    dst[x] = over(dst[x], r->colors[(int)(t * (r->count - 1) + 0.5f)]);
  }
}

void cr_polygon(const float *xy, int count, const uint32_t *colors, int color_count, float tx, float ty, float t0) {
  if (count < 3 || color_count < 1) return;
  uint32_t premultiplied[color_count];
  cr_premultiply(premultiplied, colors, (size_t)color_count);
  // t at the screen's pixels, before the transform
  ramp paint = {premultiplied, color_count, tx / T.scale, ty / T.scale, t0 - (tx * T.tx + ty * T.ty) / T.scale};
  point points[count];
  for (int i = 0; i < count; i++) points[i] = (point){to_x(xy[2 * i]), to_y(xy[2 * i + 1])};
  fill_polygon(points, count, fill_ramp, &paint);
}

// colors interpolated straight across a triangle, like GL: channel s at (x, y) is cx[s] * x + cy[s] * y + c0[s]
typedef struct {
  float cx[4], cy[4], c0[4];
} color_planes;

static void fill_planes(uint32_t *dst, int x0, int x1, int y, const void *paint) {
  const color_planes *planes = paint;
  float channels[4];
  for (int s = 0; s < 4; s++) channels[s] = planes->cx[s] * (x0 + 0.5f) + planes->cy[s] * (y + 0.5f) + planes->c0[s];
  for (int x = x0; x < x1; x++) {
    uint32_t c = 0;
    for (int s = 0; s < 4; s++) {
      float channel = channels[s] + planes->cx[s] * (x - x0);
      c |= (uint32_t)CLAMP(channel, 0.0f, 255.0f) << (8 * s);
    }
    dst[x] = over(dst[x], premultiply(c));
  }
}

// a triangle with a color at each corner
static void triangle(point a, point b, point c, uint32_t color_a, uint32_t color_b, uint32_t color_c) {
  const point corners[3] = {a, b, c};
  if (color_a == color_b && color_b == color_c) {
    uint32_t premultiplied = premultiply(color_a);
    ramp paint = {&premultiplied, 1, 0.0f, 0.0f, 0.0f};
    fill_polygon(corners, 3, fill_ramp, &paint);
    return;
  }
  float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (fabsf(area) < 1e-6f) return;

  // the weight of corner k at (x, y) is (wx[k] * x + wy[k] * y + w0[k]) / area, from the edge across from it
  const uint32_t colors[3] = {color_a, color_b, color_c};
  color_planes planes = {{0}, {0}, {0.5f, 0.5f, 0.5f, 0.5f}};  // the halves round
  for (int k = 0; k < 3; k++) {
    point p = corners[(k + 1) % 3];
    point q = corners[(k + 2) % 3];
    float wx = p.y - q.y;
    float wy = q.x - p.x;
    float w0 = -wy * p.y - wx * p.x;
    for (int s = 0; s < 4; s++) {
      float channel = ((colors[k] >> (8 * s)) & 255) / area;
      planes.cx[s] += wx * channel;
      planes.cy[s] += wy * channel;
      planes.c0[s] += w0 * channel;
    }
  }
  fill_polygon(corners, 3, fill_planes, &planes);
}

void cr_triangles(const float *xy, int count, const uint32_t *colors, int color_count, int mode) {
  if (count < 3 || (color_count != 1 && color_count != count)) return;
  point points[count];
  for (int i = 0; i < count; i++) points[i] = (point){to_x(xy[2 * i]), to_y(xy[2 * i + 1])};
  int step = mode == CR_TRIANGLE_LIST ? 3 : 1;
  for (int i = 2; i < count; i += step) {
    int first = mode == CR_TRIANGLE_FAN ? 0 : i - 2;
    uint32_t color_first = colors[color_count == 1 ? 0 : first];
    uint32_t color_before = colors[color_count == 1 ? 0 : i - 1];
    uint32_t color_last = colors[color_count == 1 ? 0 : i];
    triangle(points[first], points[i - 1], points[i], color_first, color_before, color_last);
  }
}

// ***** textures *****

// a texel, premultiplied: an RGBA8 pixel, or white at the alpha of an alpha mask
static inline uint32_t texel_at(const uint8_t *row_pixels, bool a8, int x) {
  if (a8) {
    uint32_t a = row_pixels[x];
    return rgba(a, a, a, a);
  }
  return ((const uint32_t *)row_pixels)[x];
}

// sampling along one axis: the two texels to mix, and the weight of the second out of 256
typedef struct {
  int first, second;
  uint32_t weight;
} taps;

// the texel coordinate u, within [lo, hi) of a texture n texels long, to its taps. false outside
static inline bool axis_taps(float u, float lo, float hi, int n, bool bilinear, taps *out) {
  if (u < lo || u >= hi) return false;
  if (bilinear) {
    float left = u - 0.5f;  // texel centers are at halves
    int i = (int)floorf(left);
    out->weight = (uint32_t)((left - i) * 256.0f);
    out->first = CLAMP(i, 0, n - 1);
    out->second = CLAMP(i + 1, 0, n - 1);
  } else {
    out->first = out->second = CLAMP((int)u, 0, n - 1);
    out->weight = 0;
  }
  return true;
}

// the source sampled at taps x and y, times the tint. inlined into the blit loops, as a call costs more than the mixing
__attribute__((always_inline)) static inline uint32_t sample(const uint8_t *src, int stride, bool a8, taps x, taps y,
                                                             uint32_t tint) {
  const uint8_t *top = src + (size_t)y.first * stride;
  const uint8_t *bottom = src + (size_t)y.second * stride;
  bool filtered = x.weight != 0 || y.weight != 0;
  if (a8) {
    uint32_t a = top[x.first];
    if (filtered) {
      uint32_t top_a = top[x.first] * (256 - x.weight) + top[x.second] * x.weight;
      uint32_t bottom_a = bottom[x.first] * (256 - x.weight) + bottom[x.second] * x.weight;
      a = (top_a * (256 - y.weight) + bottom_a * y.weight) >> 16;
    }
    return mul(tint, a);
  }
  uint32_t c = texel_at(top, false, x.first);
  if (filtered) {
    uint32_t top_right = texel_at(top, false, x.second);
    uint32_t bottom_left = texel_at(bottom, false, x.first);
    uint32_t bottom_right = texel_at(bottom, false, x.second);
    // mixing a texel with itself is that texel, as within or around an icon
    if (top_right != c || bottom_left != c || bottom_right != c) {
      uint32_t wx = x.weight * 255 >> 8;
      uint32_t wy = y.weight * 255 >> 8;
      c = lerp(lerp(c, top_right, wx), lerp(bottom_left, bottom_right, wx), wy);
    }
  }
  return tint == 0xffffffff ? c : mul4(c, tint);
}

// n texels of a source row drawn one to one over dst
static void blit_row_unscaled(uint32_t *restrict dst, const uint8_t *restrict src_row, bool a8, int n, uint32_t tint) {
  if (a8) {
    // glyphs are mostly clear, or covered where they're drawn in an opaque color
    bool opaque = alpha(tint) == 255;
    for (int i = 0; i < n; i++) {
      if (src_row[i] == 255 && opaque) {
        dst[i] = tint;
      } else if (src_row[i] != 0) {
        dst[i] = over(dst[i], mul(tint, src_row[i]));
      }
    }
  } else if (tint == 0xffffffff) {
    // icons and overlays are mostly clear or opaque
    const uint32_t *texels = (const uint32_t *)src_row;
    for (int i = 0; i < n; i++) {
      if (alpha(texels[i]) == 255) {
        dst[i] = texels[i];
      } else if (texels[i] != 0) {
        dst[i] = over(dst[i], texels[i]);
      }
    }
  } else {
    const uint32_t *texels = (const uint32_t *)src_row;
    for (int i = 0; i < n; i++) dst[i] = over(dst[i], mul4(texels[i], tint));
  }
}

// a row of an alpha mask, its columns sampled at their taps between the rows top and bottom, over dst
static void blit_row_scaled_a8(uint32_t *restrict dst, const uint8_t *top, const uint8_t *bottom, const taps *columns, int n,
                               uint32_t bottom_weight, uint32_t tint) {
  for (int i = 0; i < n; i++) {
    taps x = columns[i];
    uint32_t top_left = top[x.first];
    uint32_t top_right = top[x.second];
    uint32_t bottom_left = bottom[x.first];
    uint32_t bottom_right = bottom[x.second];
    // glyphs are mostly clear
    if ((top_left | top_right | bottom_left | bottom_right) == 0) continue;
    uint32_t top_a = top_left * (256 - x.weight) + top_right * x.weight;
    uint32_t bottom_a = bottom_left * (256 - x.weight) + bottom_right * x.weight;
    uint32_t a = (top_a * (256 - bottom_weight) + bottom_a * bottom_weight) >> 16;
    if (a != 0) dst[i] = over(dst[i], mul(tint, a));
  }
}

void cr_blit(const uint8_t *src, int src_width, int src_height, int src_stride, bool a8, float sx, float sy, float sw,
             float sh, float dx, float dy, float dw, float dh, float ox, float oy, float rotation, uint32_t tint, bool bilinear) {
  dx = to_x(dx);
  dy = to_y(dy);
  dw *= T.scale;
  dh *= T.scale;
  ox *= T.scale;
  oy *= T.scale;
  if (dw <= 0 || dh <= 0 || sw == 0 || sh == 0 || src_width <= 0 || src_height <= 0) return;
  tint = premultiply(tint);
  float cos_r = cosf(rotation * 0.01745329f);
  float sin_r = sinf(rotation * 0.01745329f);

  // the pixels the rotated destination covers, clipped
  float left = INFINITY;
  float right = -INFINITY;
  float top = INFINITY;
  float bottom = -INFINITY;
  for (int corner = 0; corner < 4; corner++) {
    float local_x = (corner & 1 ? dw : 0) - ox;
    float local_y = (corner & 2 ? dh : 0) - oy;
    float x = dx + local_x * cos_r - local_y * sin_r;
    float y = dy + local_x * sin_r + local_y * cos_r;
    left = fminf(left, x);
    right = fmaxf(right, x);
    top = fminf(top, y);
    bottom = fmaxf(bottom, y);
  }
  int x0 = MAX(first_pixel(left), T.x0);
  int x1 = MIN(first_pixel(right), T.x1);
  int y0 = MAX(first_pixel(top), T.y0);
  int y1 = MIN(first_pixel(bottom), T.y1);
  if (x0 >= x1 || y0 >= y1) return;

  // a pixel at (x, y) from the destination's top left samples the source at u = u_base + x * u_dx + y * u_dy, and v alike
  float u_scale = sw / dw;
  float v_scale = sh / dh;
  float u_dx = cos_r * u_scale;
  float u_dy = sin_r * u_scale;
  float v_dx = -sin_r * v_scale;
  float v_dy = cos_r * v_scale;
  float u_base = (sw < 0 ? sx - sw : sx) + ox * u_scale;
  float v_base = (sh < 0 ? sy - sh : sy) + oy * v_scale;
  float u_end = sx + fabsf(sw);
  float v_end = sy + fabsf(sh);

  if (rotation != 0) {
    for (int y = y0; y < y1; y++) {
      uint32_t *dst = row(y);
      float py = y + 0.5f - dy;
      for (int x = x0; x < x1; x++) {
        float px = x + 0.5f - dx;
        taps tx, ty;
        if (axis_taps(u_base + px * u_dx + py * u_dy, sx, u_end, src_width, bilinear, &tx) &&
            axis_taps(v_base + px * v_dx + py * v_dy, sy, v_end, src_height, bilinear, &ty)) {
          dst[x] = over(dst[x], sample(src, src_stride, a8, tx, ty, tint));
        }
      }
    }
    return;
  }

  // unrotated, u depends only on x and v only on y: the taps of each column are found once
  int n = 0;
  taps columns[x1 - x0];
  for (int x = x0; x < x1; x++) {
    if (axis_taps(u_base + (x + 0.5f - dx) * u_dx, sx, u_end, src_width, bilinear, &columns[n])) {
      if (n == 0) x0 = x;
      n++;
    } else if (n > 0) {
      break;
    }
  }
  if (n == 0) return;
  bool unscaled = !bilinear;
  for (int i = 1; i < n && unscaled; i++) unscaled = columns[i].first == columns[0].first + i;

  uint32_t samples[n];
  for (int y = y0; y < y1; y++) {
    if (y + 2 < y1) prefetch(row(y + 2) + x0, n);
    taps ty;
    if (!axis_taps(v_base + (y + 0.5f - dy) * v_dy, sy, v_end, src_height, bilinear, &ty)) continue;
    uint32_t *dst = row(y) + x0;
    const uint8_t *top = src + (size_t)ty.first * src_stride;
    const uint8_t *bottom = src + (size_t)ty.second * src_stride;
    if (unscaled) {
      blit_row_unscaled(dst, top + (size_t)columns[0].first * (a8 ? 1 : 4), a8, n, tint);
    } else if (a8) {
      blit_row_scaled_a8(dst, top, bottom, columns, n, ty.weight, tint);
    } else {
      for (int i = 0; i < n; i++) samples[i] = sample(src, src_stride, a8, columns[i], ty, tint);
      for (int i = 0; i < n; i++) dst[i] = over(dst[i], samples[i]);
    }
  }
}

// ***** NV12 *****

// what a conversion is made from, compared whole to reuse it
typedef struct {
  const uint8_t *y_plane, *uv_plane;
  float dx, dy, dw, dh;
  int x0, x1, y0, y1, width, height, stride, saturation;
  bool flip_x;
  uint8_t curve[1280];
} nv12_params;

// the last conversion. a frame drawn more than once, like a 20 Hz camera on a 60 Hz screen, is kept to be copied again
static struct {
  nv12_params params;
  bool redrawn;  // whether the last conversion was drawn again
  bool kept;     // whether pixels hold it
  uint32_t *pixels;
  size_t capacity;
} NV;

// the luma of a frame pixel, with its chroma pair at u and v, converted like the GPU's NV12 import (limited range BT.601).
// mixing with the luma is scaling the chroma, as the conversion keeps the luma. with saturation <= 256, channels are -277 to 536
typedef struct {
  int red, green, blue;
} chroma_offsets;

static inline chroma_offsets nv12_chroma(const uint8_t *pair, int saturation) {
  int u = (pair[0] - 128) * saturation;
  int v = (pair[1] - 128) * saturation;
  return (chroma_offsets){(409 * v) >> 16, -((100 * u + 208 * v) >> 16), (516 * u) >> 16};
}

static inline uint32_t nv12_pixel(uint8_t luma, chroma_offsets chroma, const uint8_t *curve) {
  const uint8_t *mapped = curve + 512 + (((luma - 16) * 298) >> 8);
  return rgba(mapped[chroma.red], mapped[chroma.green], mapped[chroma.blue], 255);
}

// frame row pixels lo to hi, lo even. a chroma pair is shared by two luma pixels
static void nv12_row(uint32_t *restrict dst, const uint8_t *restrict luma, const uint8_t *restrict chroma, int lo, int hi,
                     const uint8_t *restrict curve, int saturation) {
  for (int x = lo; x <= hi; x += 2) {
    chroma_offsets offsets = nv12_chroma(chroma + x, saturation);
    dst[x] = nv12_pixel(luma[x], offsets, curve);
    dst[x + 1] = nv12_pixel(luma[x + 1], offsets, curve);
  }
}

// n pixels of a frame row, each converted at its column, for fewer pixels than the row has
static void nv12_columns(uint32_t *restrict dst, const uint8_t *restrict luma, const uint8_t *restrict chroma,
                         const int *columns, int n, const uint8_t *restrict curve, int saturation) {
  for (int i = 0; i < n; i++) {
    int x = columns[i];
    dst[i] = nv12_pixel(luma[x], nv12_chroma(chroma + (x & ~1), saturation), curve);
  }
}

static bool reserve_nv12_pixels(size_t count) {
  if (count <= NV.capacity) return true;
  free(NV.pixels);
  NV.pixels = malloc(count * 4);
  NV.capacity = NV.pixels ? count : 0;
  return NV.pixels != NULL;
}

void cr_nv12(const uint8_t *y_plane, const uint8_t *uv_plane, int stride, int width, int height, float dx, float dy, float dw,
             float dh, bool flip_x, const uint8_t *curve, int saturation, bool new_frame) {
  if (new_frame) NV.kept = false;
  dx = to_x(dx);
  dy = to_y(dy);
  dw *= T.scale;
  dh *= T.scale;
  int x0 = MAX(first_pixel(dx), T.x0);
  int x1 = MIN(first_pixel(dx + dw), T.x1);
  int y0 = MAX(first_pixel(dy), T.y0);
  int y1 = MIN(first_pixel(dy + dh), T.y1);
  if (x0 >= x1 || y0 >= y1) return;
  const int n = x1 - x0;

  nv12_params params;
  memset(&params, 0, sizeof(params));  // and so its padding
  params.y_plane = y_plane;
  params.uv_plane = uv_plane;
  params.dx = dx;
  params.dy = dy;
  params.dw = dw;
  params.dh = dh;
  params.x0 = x0;
  params.x1 = x1;
  params.y0 = y0;
  params.y1 = y1;
  params.width = width;
  params.height = height;
  params.stride = stride;
  params.saturation = CLAMP(saturation, 0, 256);
  params.flip_x = flip_x;
  memcpy(params.curve, curve, sizeof(params.curve));

  bool same = !new_frame && memcmp(&params, &NV.params, sizeof(params)) == 0;
  if (same && NV.kept) {
    for (int y = y0; y < y1; y++) memcpy(row(y) + x0, NV.pixels + (size_t)(y - y0) * n, (size_t)n * 4);
    NV.redrawn = true;
    return;
  }
  bool keep = (same || NV.redrawn) && reserve_nv12_pixels((size_t)n * (y1 - y0));
  NV.redrawn = same;
  NV.params = params;

  // converted straight onto the screen, at the frame column each pixel samples, nearest
  int columns[n];
  for (int i = 0; i < n; i++) {
    int column = CLAMP((int)((x0 + i + 0.5f - dx) * width / dw), 0, width - 1);
    columns[i] = flip_x ? width - 1 - column : column;
  }
  // when there are fewer pixels than frame columns between the first and last sampled, each pixel is converted. else the
  // frame row is, in whole chroma pairs, and sampled
  int lo = MIN(columns[0], columns[n - 1]) & ~1;
  int hi = MAX(columns[0], columns[n - 1]);
  bool downscaled = n < hi - lo;
  uint32_t converted[downscaled ? 1 : hi + 2];
  int last_frame_row = -1;
  for (int y = y0; y < y1; y++) {
    uint32_t *dst = row(y) + x0;
    int frame_row = CLAMP((int)((y + 0.5f - dy) * height / dh), 0, height - 1);
    if (frame_row == last_frame_row) {  // upscaled, the same frame row again
      memcpy(dst, row(y - 1) + x0, (size_t)n * 4);
    } else {
      const uint8_t *luma = y_plane + (size_t)frame_row * stride;
      const uint8_t *chroma = uv_plane + (size_t)(frame_row / 2) * stride;
      if (downscaled) {
        nv12_columns(dst, luma, chroma, columns, n, curve, params.saturation);
      } else {
        nv12_row(converted, luma, chroma, lo, hi, curve, params.saturation);
        for (int i = 0; i < n; i++) dst[i] = converted[columns[i]];
      }
      last_frame_row = frame_row;
    }
    if (keep) memcpy(NV.pixels + (size_t)(y - y0) * n, dst, (size_t)n * 4);
  }
  NV.kept = keep;
}
