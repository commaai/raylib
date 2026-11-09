import unittest

import numpy as np

import raylib_nb as rl

cpu = rl.cpu


def target(w, h):
  fb = np.zeros((h, w, 4), np.uint8)
  cpu.target(fb, w, h, w * 4)
  return fb


class TestCpu(unittest.TestCase):
  def test_rect(self):
    fb = target(8, 8)
    background = rl.Color(10, 20, 30, 255)
    cpu.rect(0, 0, 8, 8, background, background, background, background)
    self.assertEqual(fb[0, 0].tolist(), [10, 20, 30, 255])
    c = rl.Color(255, 0, 0, 255)
    cpu.rect(2, 2, 4, 4, c, c, c, c)
    self.assertEqual(fb[2, 2].tolist(), [255, 0, 0, 255])
    self.assertEqual(fb[4, 4].tolist(), [10, 20, 30, 255])

  def test_rect_blends_straight_alpha(self):
    fb = target(1, 1)
    white, half_red = rl.Color(255, 255, 255, 255), rl.Color(255, 0, 0, 128)
    cpu.rect(0, 0, 1, 1, white, white, white, white)
    cpu.rect(0, 0, 1, 1, half_red, half_red, half_red, half_red)
    self.assertEqual(fb[0, 0].tolist(), [255, 127, 127, 255])

  def test_shared_horizontal_edge_blends_once(self):
    fb = target(20, 20)
    # a quad split along y = 5.5, which passes through pixel centers
    pts = np.array([[2, 2], [12, 2], [2, 5.5], [12, 5.5], [2, 9], [12, 9]], np.float32)
    cpu.triangles(pts, np.array([0x80ffffff], np.uint32), cpu.TRIANGLE_STRIP)
    self.assertEqual(set(fb[2:9, 2:12, 3].ravel().tolist()), {128})

  def test_polygon_blends_once_where_it_overlaps_itself(self):
    fb = target(8, 8)
    square = [[1, 1], [5, 1], [5, 5], [1, 5]]
    cpu.polygon(np.array(square + square, np.float32), np.array([0x80ffffff], np.uint32))
    self.assertEqual(fb[:, :, 3].tolist(), [[128 if 1 <= x < 5 and 1 <= y < 5 else 0 for x in range(8)] for y in range(8)])

  def test_polygon_ramp(self):
    fb = target(4, 1)
    red, blue = 0xff0000ff, 0xffff0000
    cpu.polygon(np.array([[0, 0], [4, 0], [4, 1], [0, 1]], np.float32), np.array([red, blue], np.uint32), tx=0.25)
    self.assertEqual(fb.view(np.uint32).ravel().tolist(), [red, red, blue, blue])

  def test_blit_samples_clamped_source(self):
    fb = target(4, 1)
    src = np.array([0xff000011, 0xff000022, 0xff000033, 0xff000044], np.uint32)
    white = rl.Color(255, 255, 255, 255)
    cpu.blit(src, 4, 1, 16, False, rl.Rectangle(-2, 0, 8, 1), rl.Rectangle(0, 0, 4, 1), rl.Vector2(0, 0), 0, white, False)
    self.assertEqual(fb.view(np.uint32).ravel().tolist(), [0xff000011, 0xff000022, 0xff000044, 0xff000044])

  def test_nv12_gray(self):
    fb = target(4, 4)
    frame = np.full(4 * 4 + 4 * 2, 128, np.uint8)
    frame[:16] = 126  # limited range luma 126 is 128
    cpu.nv12(frame, 16, 4, 4, 4, rl.Rectangle(0, 0, 4, 4), False, np.clip(np.arange(-512, 768), 0, 255).astype(np.uint8), 256, True)
    self.assertEqual(fb[1, 1].tolist(), [128, 128, 128, 255])

  def test_checks_buffer_sizes(self):
    with self.assertRaises(ValueError):
      cpu.target(np.zeros(10, np.uint8), 8, 8, 32)
    target(4, 4)
    with self.assertRaises(ValueError):
      cpu.blit(np.zeros(4, np.uint8), 4, 4, 16, False, rl.Rectangle(0, 0, 4, 4), rl.Rectangle(0, 0, 4, 4), rl.Vector2(0, 0), 0,
               rl.Color(255, 255, 255, 255), False)
    with self.assertRaises(TypeError):
      cpu.triangles(np.zeros(6, np.float64), np.zeros(1, np.uint32), cpu.TRIANGLE_LIST)

  def test_no_target_without_software_renderer(self):
    if not cpu.software:
      with self.assertRaises(RuntimeError):
        cpu.target_raylib()


if __name__ == "__main__":
  unittest.main()
