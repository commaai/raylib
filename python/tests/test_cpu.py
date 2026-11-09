import unittest

import numpy as np

import raylib_nb as rl

cpu = rl.cpu


def target(w, h):
  fb = np.zeros((h, w, 4), np.uint8)
  cpu.target(fb, w, h, w * 4)
  return fb


class TestCpu(unittest.TestCase):
  def test_clear_and_rect(self):
    fb = target(8, 8)
    cpu.clear(rl.Color(10, 20, 30, 255))
    self.assertEqual(fb[0, 0].tolist(), [10, 20, 30, 255])
    c = rl.Color(255, 0, 0, 255)
    cpu.rect(2, 2, 4, 4, c, c, c, c)
    self.assertEqual(fb[2, 2].tolist(), [255, 0, 0, 255])
    self.assertEqual(fb[4, 4].tolist(), [10, 20, 30, 255])

  def test_shared_horizontal_edge_blends_once(self):
    fb = target(20, 20)
    # a quad split along y = 5.5, which passes through pixel centers
    pts = np.array([[2, 2], [12, 2], [2, 5.5], [12, 5.5], [2, 9], [12, 9]], np.float32)
    cpu.triangles(pts, np.array([0x80ffffff], np.uint32), 1)
    self.assertEqual(set(fb[2:9, 2:12, 3].ravel().tolist()), {128})

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
      cpu.triangles(np.zeros(6, np.float64), np.zeros(1, np.uint32), 0)

  def test_no_display_off_device(self):
    if rl._backend.backend != "comma":
      self.assertLess(cpu.display_init(-1, False), 0)
      self.assertIsNone(cpu.display_target(4, 4))
      self.assertLess(cpu.display_blob("SDE_DSPP_GC_V1", None), 0)


if __name__ == "__main__":
  unittest.main()
