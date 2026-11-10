// rcpu draws the 2D primitives of raylib's software renderer, rlsw, in its place: what's alpha blended onto the screen,
// in one color per vertex or from a texture drawn whole into a rectangle. the rest, like render textures, is left to rlsw.
// RLSW_RASTER_ONLY=1 leaves everything to rlsw.
//
// included by rcore.c, after rlsw and the platform, as rlsw calls its hooks with its own types and state.
// a platform that lends rlsw its framebuffer's pixels defines ReturnScreenPixels to take them back
#include <errno.h>

#if defined(GRAPHICS_API_OPENGL_SOFTWARE)

#if !defined(PLATFORM_COMMA)
static void ReturnScreenPixels(void) {}
#endif

// the texture rcpu draws into, until it changes
static const sw_texture_t *targeted;

// target what rlsw draws into, top down as its rows are bottom up like GL's, clipped to its scissor
static void target_color_buffer(void) {
  const sw_texture_t *buffer = RLSW.colorBuffer;
  targeted = buffer;
  uint8_t *top_row = (uint8_t *)buffer->pixels + (size_t)(buffer->height - 1) * buffer->width * 4;
  cr_target(top_row, buffer->width, buffer->height, -buffer->width * 4);
  if (RLSW.userState & SW_STATE_SCISSOR_TEST) {
    cr_clip(RLSW.scMin[0], buffer->height - RLSW.scMax[1], RLSW.scMax[0], buffer->height - RLSW.scMin[1]);
  }
}

int cr_target_raylib(void) {
  if (!RLSW.colorBuffer || RLSW.colorBuffer->format != SW_PIXELFORMAT_COLOR_R8G8B8A8) return -ENOTSUP;
  float m[16];  // column major
  swGetFloatv(SW_MODELVIEW_MATRIX, m);
  bool scale_and_translate = m[1] == 0 && m[4] == 0 && m[0] == m[5] && m[0] > 0;
  if (!scale_and_translate) return -EINVAL;
  rlDrawRenderBatchActive();
  target_color_buffer();
  cr_transform(m[0], m[12], m[13]);
  return 0;
}

// rcpu colors are packed r | g << 8 | b << 16 | a << 24, rlsw's are floats from 0 to 1
static uint32_t pack_color(const float *color) {
  uint32_t packed = 0;
  for (int channel = 0; channel < 4; channel++) {
    float value = fminf(fmaxf(color[channel], 0.0f), 1.0f);
    uint32_t byte = (uint32_t)(value * 255.0f + 0.5f);
    packed |= byte << (8 * channel);
  }
  return packed;
}

static bool is_gray_alpha(const sw_texture_t *tex) { return tex->format == SW_PIXELFORMAT_COLOR_GRAYALPHA; }

static bool is_white(const sw_texture_t *tex, int x, int y) {
  const uint8_t *pixels = tex->pixels;
  if (is_gray_alpha(tex)) {
    const uint8_t *texel = pixels + ((size_t)y * tex->width + x) * 2;
    return texel[0] == 255 && texel[1] == 255;
  }
  const uint8_t *texel = pixels + ((size_t)y * tex->width + x) * 4;
  return texel[0] == 255 && texel[1] == 255 && texel[2] == 255 && texel[3] == 255;
}

// whether every texcoord is within a few white texels. raylib's shapes are textured like that, with white texels of the
// default font: one with FLAG_MSAA_4X_HINT, else a few
static bool samples_only_white(const sw_texture_t *tex, const sw_vertex_t *vertices, int count) {
  if (tex->format != SW_PIXELFORMAT_COLOR_R8G8B8A8 && !is_gray_alpha(tex)) return false;
  float u_min = vertices[0].texcoord[0], u_max = u_min;
  float v_min = vertices[0].texcoord[1], v_max = v_min;
  for (int i = 1; i < count; i++) {
    u_min = fminf(u_min, vertices[i].texcoord[0]);
    u_max = fmaxf(u_max, vertices[i].texcoord[0]);
    v_min = fminf(v_min, vertices[i].texcoord[1]);
    v_max = fmaxf(v_max, vertices[i].texcoord[1]);
  }
  // the texels covered, with texcoords on a texel's edge rounded off it
  int x0 = (int)floorf(u_min * tex->width + 1e-3f), x1 = (int)ceilf(u_max * tex->width - 1e-3f);
  int y0 = (int)floorf(v_min * tex->height + 1e-3f), y1 = (int)ceilf(v_max * tex->height - 1e-3f);
  x1 = x1 > x0 ? x1 : x0 + 1;
  y1 = y1 > y0 ? y1 : y0 + 1;
  bool few = (x1 - x0) * (y1 - y0) <= 64;
  if (!few || x0 < 0 || y0 < 0 || x1 > tex->width || y1 > tex->height) return false;

  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      if (!is_white(tex, x, y)) return false;
    }
  }
  return true;
}

typedef struct {
  float x, y;
} point;

// a polygon that is an axis aligned rectangle: the vertices at its corners
typedef struct {
  int top_left, top_right, bottom_left, bottom_right;
} rect_corners;

static bool find_rect(const point *points, int count, rect_corners *rect) {
  if (count != 4) return false;
  float left = points[0].x, right = left, top = points[0].y, bottom = top;
  for (int i = 1; i < 4; i++) {
    left = fminf(left, points[i].x);
    right = fmaxf(right, points[i].x);
    top = fminf(top, points[i].y);
    bottom = fmaxf(bottom, points[i].y);
  }
  int found[2][2] = {{-1, -1}, {-1, -1}};  // [is bottom][is right]
  for (int i = 0; i < 4; i++) {
    bool on_x_edge = points[i].x == left || points[i].x == right;
    bool on_y_edge = points[i].y == top || points[i].y == bottom;
    if (!on_x_edge || !on_y_edge) return false;
    int is_bottom = points[i].y == bottom && bottom != top;
    int is_right = points[i].x == right && right != left;
    if (found[is_bottom][is_right] >= 0) return false;  // a corner twice, so some other corner is missing
    found[is_bottom][is_right] = i;
  }
  rect->top_left = found[0][0];
  rect->top_right = found[0][1];
  rect->bottom_left = found[1][0];
  rect->bottom_right = found[1][1];
  return true;
}

// ***** textures *****

// a texture as rcpu blits it: premultiplied RGBA8, or an alpha mask. drawn minified, the texture is sampled from a
// smaller level of it, each half the size of the last, like GL's mipmaps
#define MAX_LEVELS 12

typedef struct {
  const uint8_t *pixels;
  int width, height;
} blit_level;

typedef struct {
  bool ready;
  bool a8;
  int level_count;              // levels made so far, the first is the full size texture
  blit_level levels[MAX_LEVELS];
  uint8_t *copies[MAX_LEVELS];  // the pixels made for each level. the first is the texture's own when they're the same
} blit_source;

// by texture pool slot, made when first drawn and dropped when the texture changes
static blit_source blit_sources[SW_MAX_TEXTURES];

static int texture_slot(const sw_texture_t *tex) {
  const sw_pool_t *pool = &RLSW.texturePool;
  const char *first = pool->data, *slot = (const char *)tex;
  bool in_pool = first && slot >= first && slot < first + (size_t)pool->capacity * pool->stride;
  return in_pool ? (int)((size_t)(slot - first) / pool->stride) : -1;
}

static void drop_masks(void);  // the alpha masks resampled from textures, see below

static void cr_sw_texture_changed(const sw_texture_t *tex) {
  if (tex == &RLSW.framebuffer.color) ReturnScreenPixels();
  if (tex == targeted) {  // its pixels are going, until targeted again
    cr_target(NULL, 0, 0, 0);
    targeted = NULL;
  }
  int slot = texture_slot(tex);
  if (slot < 0) return;
  if (blit_sources[slot].a8) drop_masks();
  for (int i = 0; i < MAX_LEVELS; i++) free(blit_sources[slot].copies[i]);
  blit_sources[slot] = (blit_source){0};
}

// rlsw draws into the color attachments of framebuffers, behind the back of a copy
static bool is_render_target(const sw_texture_t *tex) {
  const sw_pool_t *framebuffers = &RLSW.framebufferPool;
  for (int handle = 1; handle < framebuffers->watermark; handle++) {
    const sw_framebuffer_t *fb = sw_pool_get(framebuffers, handle);
    if (fb && sw_pool_get(&RLSW.texturePool, fb->colorAttachment) == tex) return true;
  }
  return false;
}

static bool is_blittable(const sw_texture_t *tex) {
  switch (tex->format) {
    case SW_PIXELFORMAT_COLOR_R8G8B8A8:
    case SW_PIXELFORMAT_COLOR_R8G8B8:
    case SW_PIXELFORMAT_COLOR_GRAYALPHA:
    case SW_PIXELFORMAT_COLOR_GRAYSCALE:
      return true;
    default:
      return false;
  }
}

static inline uint32_t opaque(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16 | 0xffu << 24; }

// the full size level, premultiplied
static uint8_t *first_level(const sw_texture_t *tex, bool *a8) {
  const size_t count = (size_t)tex->width * tex->height;
  *a8 = false;
  if (tex->format == SW_PIXELFORMAT_COLOR_R8G8B8A8) {
    // opaque and clear pixels are the same premultiplied
    const uint32_t *pixels = tex->pixels;
    bool premultiplied = true;
    for (size_t i = 0; i < count && premultiplied; i++) premultiplied = pixels[i] >> 24 == 255 || pixels[i] == 0;
    if (premultiplied) return tex->pixels;
    uint32_t *copy = malloc(count * 4);
    if (copy) cr_premultiply(copy, pixels, count);
    return (uint8_t *)copy;
  }
  if (tex->format == SW_PIXELFORMAT_COLOR_R8G8B8 || tex->format == SW_PIXELFORMAT_COLOR_GRAYSCALE) {
    const uint8_t *pixels = tex->pixels;
    uint32_t *copy = malloc(count * 4);
    if (!copy) return NULL;
    bool gray = tex->format == SW_PIXELFORMAT_COLOR_GRAYSCALE;
    for (size_t i = 0; i < count; i++) {
      copy[i] = gray ? opaque(pixels[i], pixels[i], pixels[i]) : opaque(pixels[3 * i], pixels[3 * i + 1], pixels[3 * i + 2]);
    }
    return (uint8_t *)copy;
  }

  // gray and alpha, like the glyphs of fonts. when all white, it's an alpha mask
  const uint8_t *pixels = tex->pixels;
  bool white = true;
  for (size_t i = 0; i < count && white; i++) white = pixels[2 * i] == 255;
  uint8_t *copy = malloc(count * (white ? 1 : 4));
  if (!copy) return NULL;
  if (white) {
    for (size_t i = 0; i < count; i++) copy[i] = pixels[2 * i + 1];
  } else {
    uint32_t *colors = (uint32_t *)copy;
    for (size_t i = 0; i < count; i++) {
      uint32_t gray = pixels[2 * i], a = pixels[2 * i + 1];
      colors[i] = gray | gray << 8 | gray << 16 | a << 24;
    }
    cr_premultiply(colors, colors, count);
  }
  *a8 = white;
  return copy;
}

// the level half the size of the last one made, each texel the average of the four it covers
static bool make_next_level(blit_source *source) {
  const blit_level *larger = &source->levels[source->level_count - 1];
  const int bytes = source->a8 ? 1 : 4;
  const int width = larger->width > 1 ? larger->width / 2 : 1, height = larger->height > 1 ? larger->height / 2 : 1;
  uint8_t *pixels = malloc((size_t)width * height * bytes);
  if (!pixels) return false;
  for (int y = 0; y < height; y++) {
    const uint8_t *top = larger->pixels + (size_t)(2 * y) * larger->width * bytes;
    const uint8_t *bottom = larger->height > 1 ? top + (size_t)larger->width * bytes : top;
    uint8_t *out = pixels + (size_t)y * width * bytes;
    for (int x = 0; x < width; x++) {
      int left = 2 * x * bytes, right = larger->width > 1 ? left + bytes : left;
      for (int c = 0; c < bytes; c++) {
        out[x * bytes + c] = (uint8_t)((top[left + c] + top[right + c] + bottom[left + c] + bottom[right + c] + 2) >> 2);
      }
    }
  }
  source->copies[source->level_count] = pixels;
  source->levels[source->level_count] = (blit_level){pixels, width, height};
  source->level_count++;
  return true;
}

// the texture ready to blit, or NULL to leave it to rlsw
static blit_source *get_blit_source(const sw_texture_t *tex) {
  int slot = texture_slot(tex);
  if (!is_blittable(tex) || slot < 0 || is_render_target(tex)) return NULL;
  blit_source *source = &blit_sources[slot];
  if (!source->ready) {
    uint8_t *pixels = first_level(tex, &source->a8);
    if (!pixels) return NULL;
    if (pixels != tex->pixels) source->copies[0] = pixels;
    source->levels[0] = (blit_level){pixels, tex->width, tex->height};
    source->level_count = 1;
    source->ready = true;
  }
  return source;
}

// the level to sample a source minified by a factor: the smallest that's still at least as large as the destination
static const blit_level *get_level(blit_source *source, float minification) {
  int level = 0;
  for (; minification >= 2.0f && level + 1 < MAX_LEVELS; minification *= 0.5f) {
    const blit_level *current = &source->levels[level];
    if (current->width == 1 && current->height == 1) break;
    if (level + 1 == source->level_count && !make_next_level(source)) break;
    level++;
  }
  return &source->levels[level];
}

// ***** scaled masks *****

// an alpha mask drawn scaled and unrotated, which is how raylib draws text, is resampled once for its size and where it
// falls within a pixel, and copied each time it's drawn: resampling costs several times what copying does
#define MASK_PHASES 4                // where a mask can fall within a pixel, along each axis
#define MASK_SIZE_STEP 256           // sizes are a 1/256 apart, as they come from raylib slightly off by where they're drawn
#define MASK_SLOTS 4096              // a power of two
#define MASK_BYTES ((size_t)8 << 20)  // when the masks outgrow it, they're all dropped

typedef struct {
  int texture_slot, level;
  float src_x, src_y, src_width, src_height, width, height;
  int phase_x, phase_y;
} mask_key;

typedef struct {
  mask_key key;
  uint8_t *pixels;  // NULL when the slot is free
  int width, height;
} scaled_mask;

static struct {
  scaled_mask slots[MASK_SLOTS];
  int count;
  size_t bytes;
} masks;

static float mask_size(float size) { return roundf(size * MASK_SIZE_STEP) / MASK_SIZE_STEP; }

static void drop_masks(void) {
  for (int i = 0; i < MASK_SLOTS; i++) free(masks.slots[i].pixels);
  memset(&masks, 0, sizeof(masks));
}

static uint32_t hash_key(const mask_key *key) {
  const uint8_t *bytes = (const uint8_t *)key;
  uint32_t hash = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < sizeof(*key); i++) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

// the slot of the mask with this key, or the free slot for it
static scaled_mask *find_mask(const mask_key *key) {
  for (uint32_t i = hash_key(key);; i++) {
    scaled_mask *mask = &masks.slots[i & (MASK_SLOTS - 1)];
    if (!mask->pixels || memcmp(&mask->key, key, sizeof(*key)) == 0) return mask;
  }
}

// the key's source rectangle of the level drawn into a clear target at its phase, keeping the alpha
static bool resample_mask(scaled_mask *mask, const mask_key *key, const blit_level *level) {
  float x = (float)key->phase_x / MASK_PHASES, y = (float)key->phase_y / MASK_PHASES;
  int width = (int)ceilf(x + key->width), height = (int)ceilf(y + key->height);
  size_t count = (size_t)width * height;
  uint32_t *drawn = calloc(count, 4);
  uint8_t *pixels = malloc(count);
  if (!drawn || !pixels) {
    free(drawn);
    free(pixels);
    return false;
  }
  cr_target((uint8_t *)drawn, width, height, width * 4);
  cr_blit(level->pixels, level->width, level->height, level->width, true, key->src_x, key->src_y, key->src_width,
          key->src_height, x, y, key->width, key->height, 0, 0, 0, 0xffffffff, true);
  for (size_t i = 0; i < count; i++) pixels[i] = drawn[i] >> 24;
  free(drawn);

  *mask = (scaled_mask){*key, pixels, width, height};
  masks.count++;
  masks.bytes += count;
  return true;
}

// the source rectangle of an alpha mask's level drawn scaled from (x, y), from its resampled copy. false when out of memory
static bool draw_scaled_mask(int texture_slot, const blit_source *source, const blit_level *level, float src_x, float src_y,
                             float src_width, float src_height, float x, float y, float width, float height, uint32_t tint) {
  float left = floorf(x), top = floorf(y);
  int phase_x = (int)lroundf((x - left) * MASK_PHASES), phase_y = (int)lroundf((y - top) * MASK_PHASES);
  if (phase_x == MASK_PHASES) {
    phase_x = 0;
    left += 1.0f;
  }
  if (phase_y == MASK_PHASES) {
    phase_y = 0;
    top += 1.0f;
  }
  mask_key key = {texture_slot, (int)(level - source->levels), mask_size(src_x), mask_size(src_y), mask_size(src_width),
                  mask_size(src_height), mask_size(width), mask_size(height), phase_x, phase_y};

  scaled_mask *mask = find_mask(&key);
  if (!mask->pixels) {
    if (masks.count >= MASK_SLOTS * 3 / 4 || masks.bytes >= MASK_BYTES) {
      drop_masks();
      mask = find_mask(&key);
    }
    if (!resample_mask(mask, &key, level)) return false;
  }

  target_color_buffer();
  cr_blit(mask->pixels, mask->width, mask->height, mask->width, true, 0, 0, mask->width, mask->height, left, top,
          mask->width, mask->height, 0, 0, 0, tint, false);
  return true;
}

static float distance(point a, point b) { return hypotf(b.x - a.x, b.y - a.y); }

// a textured quad as a blit, when it's a whole source rectangle drawn into a rectangle, rotated or mirrored.
// clipped, it can have more vertices along the source's edges, and texcoords a little off them. false to leave it to rlsw
static bool blit_quad(const sw_texture_t *tex, const sw_vertex_t *vertices, const point *points, int count, uint32_t tint) {
  if (count < 4) return false;
  float u_min = vertices[0].texcoord[0], u_max = u_min, v_min = vertices[0].texcoord[1], v_max = v_min;
  for (int i = 1; i < count; i++) {
    u_min = fminf(u_min, vertices[i].texcoord[0]);
    u_max = fmaxf(u_max, vertices[i].texcoord[0]);
    v_min = fminf(v_min, vertices[i].texcoord[1]);
    v_max = fmaxf(v_max, vertices[i].texcoord[1]);
  }
  if (u_min == u_max || v_min == v_max) return false;

  // the vertices at the corners of the source: [v is max][u is max]. every vertex is on its edges, within a hundredth of
  // a texel, so with all four corners the polygon is the whole source
  const float u_slack = 0.01f / tex->width, v_slack = 0.01f / tex->height;
  int at[2][2] = {{-1, -1}, {-1, -1}};
  for (int i = 0; i < count; i++) {
    float u = vertices[i].texcoord[0], v = vertices[i].texcoord[1];
    bool u_is_min = u - u_min <= u_slack, u_is_max = u_max - u <= u_slack;
    bool v_is_min = v - v_min <= v_slack, v_is_max = v_max - v <= v_slack;
    bool on_u_edge = u_is_min || u_is_max, on_v_edge = v_is_min || v_is_max;
    if (!on_u_edge && !on_v_edge) return false;
    if (on_u_edge && on_v_edge) at[v_is_max][u_is_max] = i;
  }
  for (int corner = 0; corner < 4; corner++) {
    if (at[corner >> 1][corner & 1] < 0) return false;
  }
  point source_top_left = points[at[0][0]], source_top_right = points[at[0][1]];
  point source_bottom_left = points[at[1][0]], source_bottom_right = points[at[1][1]];

  // the source's axes on the screen must be perpendicular, and make a parallelogram
  float ux = source_top_right.x - source_top_left.x, uy = source_top_right.y - source_top_left.y;
  float vx = source_bottom_left.x - source_top_left.x, vy = source_bottom_left.y - source_top_left.y;
  float width = distance(source_top_left, source_top_right), height = distance(source_top_left, source_bottom_left);
  if (width == 0 || height == 0) return false;
  bool perpendicular = fabsf(ux * vx + uy * vy) <= 1e-3f * width * height;
  bool parallelogram = fabsf(source_top_left.x + ux + vx - source_bottom_right.x) < 1e-2f &&
                       fabsf(source_top_left.y + uy + vy - source_bottom_right.y) < 1e-2f;
  if (!perpendicular || !parallelogram) return false;

  // the destination is rotated around its top left. mirrored, its top left is the source's top right and the source is flipped
  bool mirrored = ux * vy - uy * vx < 0;
  point origin = mirrored ? source_top_right : source_top_left;
  float rotation = mirrored ? atan2f(-uy, -ux) : atan2f(uy, ux);
  float src_x = u_min * tex->width, src_y = v_min * tex->height;
  float src_width = (u_max - u_min) * tex->width, src_height = (v_max - v_min) * tex->height;

  // drawn unscaled onto whole pixels, each pixel center samples a texel center: filtering would only cost time
  bool on_pixels = fabsf(origin.x - roundf(origin.x)) < 1e-3f && fabsf(origin.y - roundf(origin.y)) < 1e-3f;
  bool unscaled = fabsf(src_width - width) <= 1e-3f && fabsf(src_height - height) <= 1e-3f;
  bool magnified = width >= src_width && height >= src_height;
  SWfilter filter = magnified ? tex->magFilter : tex->minFilter;
  bool bilinear = filter == SW_LINEAR && !(rotation == 0 && unscaled && on_pixels);

  blit_source *source = get_blit_source(tex);
  if (!source) return false;
  const blit_level *level = bilinear ? get_level(source, fminf(src_width / width, src_height / height)) : &source->levels[0];
  float level_x = (float)level->width / tex->width, level_y = (float)level->height / tex->height;
  src_x *= level_x;
  src_y *= level_y;
  src_width *= level_x;
  src_height *= level_y;
  bool text_like = source->a8 && bilinear && rotation == 0 && !mirrored;
  if (text_like && draw_scaled_mask(texture_slot(tex), source, level, src_x, src_y, src_width, src_height, origin.x,
                                    origin.y, width, height, tint)) {
    return true;
  }
  int stride = level->width * (source->a8 ? 1 : 4);
  cr_blit(level->pixels, level->width, level->height, stride, source->a8, src_x, src_y, mirrored ? -src_width : src_width,
          src_height, origin.x, origin.y, width, height, 0, 0, rotation * RAD2DEG, tint, bilinear);
  return true;
}

// ***** the hook *****

static bool raster_hook_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *only_rlsw = getenv("RLSW_RASTER_ONLY");
    enabled = !(only_rlsw && strcmp(only_rlsw, "1") == 0);
  }
  return enabled;
}

// rlsw's primitive, clipped and in screen space, drawn by rcpu instead when it returns true
static bool cr_sw_raster(uint32_t state) {
  if (!raster_hook_enabled()) return false;
  // rlsw turns blending off for opaque primitives, which rcpu draws the same blended
  bool alpha_blended = (RLSW.userState & SW_STATE_BLEND) && RLSW.srcFactor == SW_SRC_ALPHA && RLSW.dstFactor == SW_ONE_MINUS_SRC_ALPHA;
  // render textures can be transparent, where rcpu's premultiplied blending differs from GL's
  bool on_screen = RLSW.colorBuffer == &RLSW.framebuffer.color;
  if (!alpha_blended || !on_screen || (state & SW_STATE_DEPTH_TEST)) return false;

  const sw_vertex_t *vertices = RLSW.primitive.buffer;
  const int count = RLSW.primitive.vertexCount;
  const sw_texture_t *tex = (state & SW_STATE_TEXTURE_2D) ? RLSW.boundTexture : NULL;
  if (tex && samples_only_white(tex, vertices, count)) tex = NULL;

  // rlsw puts pixel centers at integers and rows bottom up, rcpu at halves and top down
  int width, height;
  swGetColorBuffer(&width, &height);
  point points[SW_MAX_CLIPPED_POLYGON_VERTICES];
  uint32_t colors[SW_MAX_CLIPPED_POLYGON_VERTICES];
  bool one_color = true;
  for (int i = 0; i < count; i++) {
    bool affine = vertices[i].position[3] == 1.0f;
    if (!affine) return false;
    points[i].x = vertices[i].position[0] - 0.5f;
    points[i].y = height - (vertices[i].position[1] - 0.5f);
    colors[i] = pack_color(vertices[i].color);
    if (colors[i] != colors[0]) one_color = false;
  }

  target_color_buffer();
  if (tex) return one_color && blit_quad(tex, vertices, points, count, colors[0]);
  rect_corners rect;
  if (find_rect(points, count, &rect)) {
    cr_rect(points[rect.top_left].x, points[rect.top_left].y, points[rect.bottom_right].x, points[rect.bottom_right].y,
            colors[rect.top_left], colors[rect.top_right], colors[rect.bottom_left], colors[rect.bottom_right]);
  } else if (one_color) {
    cr_polygon(&points[0].x, count, colors, 1, 0, 0, 0);
  } else {
    cr_triangles(&points[0].x, count, colors, count, CR_TRIANGLE_FAN);
  }
  return true;
}

int cr_raylib_screen(uint8_t **pixels, int *width, int *height) {
  const sw_texture_t *screen = &RLSW.framebuffer.color;
  if (!screen->pixels) return -ENODEV;
  rlDrawRenderBatchActive();
  *pixels = screen->pixels;
  *width = screen->width;
  *height = screen->height;
  return 0;
}

#else
int cr_target_raylib(void) { return -ENODEV; }
int cr_raylib_screen(uint8_t **pixels, int *width, int *height) { return -ENODEV; }
#endif
