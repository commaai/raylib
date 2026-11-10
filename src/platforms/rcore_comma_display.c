// the panel of comma devices for raylib's software renderer, included by rcore_comma.c.
// rlsw draws into a landscape target, which the display's rotator turns into one of two portrait panel buffers to flip to.
// the target is rlsw's framebuffer, so its rows are bottom up like GL's, and the rotator flips them too
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

// the msm ION interface of the 4.9 kernel (linux/ion.h, linux/msm_ion.h)
typedef int ion_user_handle_t;
struct ion_allocation_data {
  size_t len, align;
  unsigned int heap_id_mask, flags;
  ion_user_handle_t handle;
};
struct ion_fd_data {
  ion_user_handle_t handle;
  int fd;
};
struct ion_handle_data {
  ion_user_handle_t handle;
};
struct ion_custom_data {
  unsigned int cmd;
  unsigned long arg;
};
struct ion_flush_data {
  ion_user_handle_t handle;
  int fd;
  void *vaddr;
  unsigned int offset, length;
};
#define ION_IOMMU_HEAP_ID 25
#define ION_FLAG_CACHED 1
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR('I', 6, struct ion_custom_data)
#define ION_IOC_CLEAN_CACHES _IOWR('M', 0, struct ion_flush_data)

typedef struct {
  int fd;
  ion_user_handle_t handle;
  uint8_t *map;
  size_t size;
} ion_buffer;

static struct {
  int ion_fd, rotator_fd;
  ion_buffer target, panel[2];
  uint32_t framebuffers[2];  // of the panel buffers, on the platform's own DRM fd
  int back;
} display = {.ion_fd = -1, .rotator_fd = -1};

static int ion_alloc(ion_buffer *buffer, size_t size, bool cached) {
  struct ion_allocation_data alloc = {.len = size, .align = 4096, .heap_id_mask = 1 << ION_IOMMU_HEAP_ID,
                                      .flags = cached ? ION_FLAG_CACHED : 0};
  if (ioctl(display.ion_fd, ION_IOC_ALLOC, &alloc)) return -errno;
  buffer->handle = alloc.handle;

  struct ion_fd_data share = {.handle = alloc.handle};
  if (ioctl(display.ion_fd, ION_IOC_SHARE, &share)) return -errno;
  buffer->fd = share.fd;
  buffer->size = size;

  void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
  if (map == MAP_FAILED) return -errno;
  buffer->map = map;
  return 0;
}

static void ion_free(ion_buffer *buffer) {
  if (buffer->map) munmap(buffer->map, buffer->size);
  if (buffer->size) close(buffer->fd);
  if (buffer->handle) ioctl(display.ion_fd, ION_IOC_FREE, &(struct ion_handle_data){.handle = buffer->handle});
  *buffer = (ion_buffer){0};
}

static void display_close(void) {
  // the framebuffers go with the platform's DRM fd
  for (int i = 0; i < 2; i++) ion_free(&display.panel[i]);
  ion_free(&display.target);
  if (display.rotator_fd >= 0) close(display.rotator_fd);
  if (display.ion_fd >= 0) close(display.ion_fd);
  memset(&display, 0, sizeof(display));
  display.ion_fd = display.rotator_fd = -1;
}

static int display_setup(void) {
  int panel_width = platform.drm.mode.hdisplay;
  int panel_height = platform.drm.mode.vdisplay;
  struct v4l2_control rotate = {.id = V4L2_CID_ROTATE, .value = platform.canonical_zero ? 270 : 90};
  struct v4l2_control flip = {.id = V4L2_CID_VFLIP, .value = 1};
  // the v4l2 and DRM fourccs are the same
  struct v4l2_format in = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                           .fmt.pix = {.width = panel_height, .height = panel_width, .pixelformat = DRM_FORMAT_ABGR8888}};
  struct v4l2_format out = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .fmt.pix = {.width = panel_width, .height = panel_height, .pixelformat = DRM_FORMAT_ABGR8888}};
  if (ioctl(display.rotator_fd, VIDIOC_S_CTRL, &rotate) || ioctl(display.rotator_fd, VIDIOC_S_CTRL, &flip)) return -errno;
  if (ioctl(display.rotator_fd, VIDIOC_S_FMT, &in) || ioctl(display.rotator_fd, VIDIOC_S_FMT, &out)) return -errno;
  if (in.fmt.pix.bytesperline != (uint32_t)panel_height * 4) return -EINVAL;  // rlsw's rows aren't padded

  // the target is read back while drawing, so it's cached and cleaned before each rotation
  int ret = ion_alloc(&display.target, in.fmt.pix.sizeimage, true);
  if (ret) return ret;
  for (int i = 0; i < 2; i++) {
    ret = ion_alloc(&display.panel[i], out.fmt.pix.sizeimage, false);
    if (ret) return ret;
    uint32_t handle;
    if (drmPrimeFDToHandle(platform.drm.render_fd, display.panel[i].fd, &handle)) return -errno;
    uint32_t handles[4] = {handle};
    uint32_t pitches[4] = {out.fmt.pix.bytesperline};
    uint32_t offsets[4] = {0};
    if (drmModeAddFB2(platform.drm.render_fd, panel_width, panel_height, DRM_FORMAT_ABGR8888, handles, pitches, offsets,
                      &display.framebuffers[i], 0)) return -errno;
  }

  struct v4l2_requestbuffers requests[2] = {{.count = 1, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR},
                                            {.count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_USERPTR}};
  for (int i = 0; i < 2; i++) {
    if (ioctl(display.rotator_fd, VIDIOC_REQBUFS, &requests[i])) return -errno;
    if (ioctl(display.rotator_fd, VIDIOC_STREAMON, &requests[i].type)) return -errno;
  }
  return 0;
}

// 0 or a negative errno
static int display_init(void) {
  display.ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
  if (display.ion_fd < 0) return -errno;
  display.rotator_fd = open("/dev/video2", O_RDWR | O_CLOEXEC);  // sde_rotator, a v4l2 mem2mem device
  int ret = display.rotator_fd < 0 ? -errno : display_setup();
  if (ret) display_close();
  return ret;
}

// the target to draw into, when it's width x height
static uint8_t *display_target(int width, int height) {
  bool fits = width == platform.drm.mode.vdisplay && height == platform.drm.mode.hdisplay;
  return fits ? display.target.map : NULL;
}

// set a property of the CRTC to a blob, or clear it when data is NULL. 0 or a negative errno
static int set_crtc_blob(const char *name, const void *data, uint32_t size) {
  drmModeObjectProperties *properties = drmModeObjectGetProperties(platform.drm.fd, platform.drm.crtc_id, DRM_MODE_OBJECT_CRTC);
  if (!properties) return -errno;
  int ret = -ENOENT;
  for (uint32_t i = 0; i < properties->count_props && ret == -ENOENT; i++) {
    drmModePropertyRes *property = drmModeGetProperty(platform.drm.fd, properties->props[i]);
    if (property && strcmp(property->name, name) == 0) {
      uint32_t blob = 0;
      if (data && drmModeCreatePropertyBlob(platform.drm.fd, data, size, &blob)) {
        ret = -errno;
      } else {
        bool set = drmModeObjectSetProperty(platform.drm.fd, platform.drm.crtc_id, DRM_MODE_OBJECT_CRTC, property->prop_id, blob) == 0;
        ret = set ? 0 : -errno;
      }
      if (blob) drmModeDestroyPropertyBlob(platform.drm.fd, blob);
    }
    drmModeFreeProperty(property);
  }
  drmModeFreeObjectProperties(properties);
  return ret;
}

static int rotator_queue(int type, int index, const ion_buffer *buffer) {
  // the rotator takes the dmabuf fd as the user pointer
  struct v4l2_buffer queued = {.type = type, .memory = V4L2_MEMORY_USERPTR, .index = index, .m.userptr = buffer->fd,
                               .length = buffer->size, .bytesused = buffer->size};
  return ioctl(display.rotator_fd, VIDIOC_QBUF, &queued) ? -errno : 0;
}

static int rotator_wait(int type) {
  struct v4l2_buffer done = {.type = type, .memory = V4L2_MEMORY_USERPTR};
  if (ioctl(display.rotator_fd, VIDIOC_DQBUF, &done)) return -errno;
  return done.flags & V4L2_BUF_FLAG_ERROR ? -EIO : 0;
}

// restarting the rotator's queues returns any buffers still in them
static void rotator_restart(void) {
  int output = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  int capture = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(display.rotator_fd, VIDIOC_STREAMOFF, &output);
  ioctl(display.rotator_fd, VIDIOC_STREAMOFF, &capture);
  ioctl(display.rotator_fd, VIDIOC_STREAMON, &output);
  ioctl(display.rotator_fd, VIDIOC_STREAMON, &capture);
}

// rotate the target into the back panel buffer, and give its framebuffer to flip to. 0 or a negative errno
static int display_rotate(uint32_t *framebuffer) {
  struct ion_flush_data flush = {.handle = display.target.handle, .vaddr = display.target.map, .length = display.target.size};
  struct ion_custom_data clean = {.cmd = ION_IOC_CLEAN_CACHES, .arg = (uintptr_t)&flush};
  if (ioctl(display.ion_fd, ION_IOC_CUSTOM, &clean)) return -errno;

  int ret = rotator_queue(V4L2_BUF_TYPE_VIDEO_OUTPUT, 0, &display.target);
  if (!ret) ret = rotator_queue(V4L2_BUF_TYPE_VIDEO_CAPTURE, display.back, &display.panel[display.back]);
  if (!ret) ret = rotator_wait(V4L2_BUF_TYPE_VIDEO_CAPTURE);
  if (!ret) ret = rotator_wait(V4L2_BUF_TYPE_VIDEO_OUTPUT);
  if (ret) {
    rotator_restart();
    return ret;
  }
  *framebuffer = display.framebuffers[display.back];
  return 0;
}

// the back panel buffer is on screen, the other one is the back now
static void display_shown(void) {
  display.back ^= 1;
}
