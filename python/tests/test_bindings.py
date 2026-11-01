import gc
import os
import subprocess
import sys
import threading
import time
import unittest
from array import array

import raylib_nb as rl


class TestRaylibBindings(unittest.TestCase):
    @unittest.skipUnless(os.getenv("RAYLIB_TEST_RENDER") == "1", "requires a graphics backend")
    def test_blocking_frames_release_gil(self):
        # Frame limiting must allow other Python threads to keep running.
        stop = threading.Event()
        ready = threading.Event()
        ticks = []

        def worker():
            ready.set()
            while not stop.is_set():
                ticks.append(None)
                time.sleep(0.001)

        rl.init_window(640, 480, "raylib GIL regression")
        rl.set_target_fps(10)
        thread = threading.Thread(target=worker)
        thread.start()
        try:
            self.assertTrue(ready.wait(1))
            before = len(ticks)
            for _ in range(3):
                rl.begin_drawing()
                rl.clear_background(rl.BLACK)
                rl.end_drawing()
            self.assertGreater(len(ticks) - before, 10)
        finally:
            stop.set()
            thread.join(timeout=1)
            rl.close_window()

    def test_invalid_backend_is_rejected(self):
        env = dict(os.environ, RAYLIB_BACKEND="not-a-backend")
        result = subprocess.run([sys.executable, "-c", "import raylib_nb"], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid RAYLIB_BACKEND: 'not-a-backend'", result.stderr)

    def test_struct_defaults_and_keywords(self):
        rect = rl.Rectangle()
        self.assertEqual((rect.x, rect.y, rect.width, rect.height), (0, 0, 0, 0))
        rect = rl.Rectangle(x=1, y=2, width=3, height=4)
        rect.width += 5
        self.assertEqual((rect.x, rect.y, rect.width, rect.height), (1, 2, 8, 4))
        point = rl.Vector2(y=3)
        self.assertEqual((point.x, point.y), (0, 3))
        color = rl.Color(12, 34, 56, 78)
        self.assertEqual((color.r, color.g, color.b, color.a), (12, 34, 56, 78))

    def test_nested_struct_mutation_and_lifetime(self):
        font = rl.Font()
        font.texture.width = 64
        texture = font.texture
        del font
        gc.collect()
        self.assertEqual(texture.width, 64)
        texture.height = 32
        target = rl.RenderTexture()
        target.texture.id = 42
        self.assertEqual(target.texture.id, 42)

    def test_collision_and_color(self):
        rect = rl.Rectangle(10, 20, 30, 40)
        self.assertTrue(rl.check_collision_point_rec(rl.Vector2(11, 21), rect))
        self.assertFalse(rl.check_collision_point_rec(rl.Vector2(9, 21), rect))
        overlap = rl.get_collision_rec(rect, rl.Rectangle(20, 30, 50, 60))
        self.assertEqual((overlap.x, overlap.y, overlap.width, overlap.height), (20, 30, 20, 30))
        rgb = rl.color_from_hsv(120, 1, 1)
        self.assertEqual((rgb.r, rgb.g, rgb.b, rgb.a), (0, 255, 0, 255))
        self.assertEqual((rl.WHITE.r, rl.WHITE.g, rl.WHITE.b, rl.WHITE.a), (255, 255, 255, 255))
        self.assertEqual(int(rl.ShaderUniformDataType.SHADER_UNIFORM_FLOAT), 0)

    def test_image_buffer_is_retained(self):
        pixels = bytearray([1, 2, 3, 4, 5, 6])
        image = rl.Image(pixels, 2, 1, 1, rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8)
        del pixels
        gc.collect()
        self.assertEqual(image.to_bytes(6), bytes([1, 2, 3, 4, 5, 6]))

    def test_borrowed_image_bounds_and_ownership(self):
        pixels = bytearray(12)
        image = rl.Image(pixels, 2, 2, 1, rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8)
        with self.assertRaises(BufferError):
            pixels.extend(b"123")
        for operation in (
            lambda: rl.unload_image(image),
            lambda: rl.image_resize(image, 1, 1),
            lambda: rl.image_flip_horizontal(image),
            lambda: rl.image_alpha_premultiply(image),
        ):
            with self.assertRaises(ValueError):
                operation()
        image.width = 20
        with self.assertRaises(ValueError):
            image.to_bytes(13)
        with self.assertRaises(ValueError):
            rl.Image(bytearray(1), 2, 2, 1, rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8)
        with self.assertRaises(ValueError):
            rl.Image(bytearray(12), -1, 2, 1, rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8)

    def test_image_unload_invalidates_data(self):
        image = rl.gen_image_color(2, 2, rl.WHITE)
        rl.unload_image(image)
        with self.assertRaises(ValueError):
            image.to_bytes(1)
        rl.unload_image(image)

    def test_pointer_argument_validation(self):
        texture = rl.Texture(0, 2, 2, 1, rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8)
        with self.assertRaises(ValueError):
            rl.update_texture(texture, bytes(1))
        with self.assertRaises(ValueError):
            rl.load_font_ex("unused.ttf", 16, [65], 2)
        with self.assertRaises(ValueError):
            rl.set_shader_value(rl.Shader(), 0, array("f"), rl.ShaderUniformDataType.SHADER_UNIFORM_VEC2)

    def test_log_callback_is_retained_and_formats_percent(self):
        messages = []

        def callback(level, message):
            messages.append((level, message))

        rl.set_trace_log_level(rl.TraceLogLevel.LOG_ALL)
        rl.set_trace_log_callback(callback)
        del callback
        gc.collect()
        try:
            rl.trace_log(rl.TraceLogLevel.LOG_INFO, "charge: 50% — ready")
            self.assertEqual(messages, [(int(rl.TraceLogLevel.LOG_INFO), "charge: 50% — ready")])
        finally:
            rl.set_trace_log_callback(None)
            rl.set_trace_log_level(rl.TraceLogLevel.LOG_INFO)
        with self.assertRaises(TypeError):
            rl.set_trace_log_callback(123)

    def test_python_backed_images_reject_mipmaps(self):
        pixels = bytearray(16)
        pixel_format = rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        with self.assertRaises(ValueError):
            rl.Image(pixels, 2, 2, 2, pixel_format)
        image = rl.Image(pixels, 2, 2, 1, pixel_format)
        image.mipmaps = 2
        with self.assertRaises(ValueError):
            image.to_bytes(16)
        with self.assertRaises(ValueError):
            rl.load_texture_from_image(image)

    def test_callback_is_released_at_shutdown(self):
        code = """
import raylib_nb as rl
color = rl.Color(1, 2, 3, 4)
def callback(level, message, retained=color):
    pass
rl.set_trace_log_callback(callback)
"""
        result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("nanobind: leaked", result.stderr)

    def test_invalid_image_allocation(self):
        for width, height in ((0, 1), (-1, 2), (2, -1), (2**30, 2**30)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                rl.gen_image_color(width, height, rl.WHITE)

    def test_cpu_image_operations(self):
        image = rl.gen_image_color(2, 3, rl.Color(10, 20, 30, 255))
        try:
            self.assertEqual(image.to_bytes(4), bytes([10, 20, 30, 255]))
            rl.image_resize(image, 4, 5)
            self.assertEqual((image.width, image.height), (4, 5))
            self.assertEqual(image.to_bytes(4), bytes([10, 20, 30, 255]))
        finally:
            rl.unload_image(image)


if __name__ == "__main__":
    unittest.main()
