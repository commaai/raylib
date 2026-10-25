# raylib-nanobind

Direct Python bindings for the raylib API used by comma applications. The distribution is named `raylib-nanobind`; import it as `raylib_nb`. This is a selected API, not a complete replacement for every function exposed by `pyray`. There is no CFFI runtime or `rl.ffi` compatibility layer.

## Build and install

Linux and macOS builds are supported. From the raylib repository root:

```sh
python -m pip install .
```

The build uses setuptools and nanobind 3.1.0 through PEP 517 build isolation. It needs C and C++17 compilers, make, Python development headers, and the system development libraries required by the selected raylib backends. It builds raylib from this checkout; it does not load another Python raylib package.

For Debian/Ubuntu, the desktop and headless builds use these development packages:

```sh
sudo apt-get install build-essential xorg-dev libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev
```

The comma backend additionally needs `libgbm-dev` and `libdrm-dev`. macOS uses the Xcode command-line tools and system frameworks; `ARCHFLAGS` and `MACOSX_DEPLOYMENT_TARGET` are forwarded to the C library build.

Choose the compiled backends with `RAYLIB_BACKENDS`:

```sh
RAYLIB_BACKENDS=headless python -m pip install .
RAYLIB_BACKENDS=desktop,headless python -m pip install .
```

C compilation uses at most four jobs by default; set `RAYLIB_BUILD_JOBS` to change that. Backend extensions build serially with separate objects.

Defaults are `desktop,headless` on Linux, with `comma` also included on Linux ARM, and `desktop` on macOS. Build selection does not select the backend used at runtime.

## Run

```python
import raylib_nb as rl

rl.init_window(640, 480, "raylib-nanobind")
try:
    while not rl.window_should_close():
        rl.begin_drawing()
        rl.clear_background(rl.BLACK)
        rl.draw_rectangle_rec(rl.Rectangle(20, 20, 120, 60), rl.WHITE)
        rl.end_drawing()
finally:
    rl.close_window()
```

At runtime, `RAYLIB_BACKEND` accepts `desktop`, `headless`, or `comma`. If unset, `/AGNOS` or `/TICI` selects `comma`; other hosts select `desktop`. The selected backend must have been built. For a headless EGL environment:

```sh
EGL_PLATFORM=surfaceless RAYLIB_BACKEND=headless python your_app.py
```

## API and ownership

Functions use snake_case names; structs retain raylib field names. `Vector2`, `Rectangle`, `Color`, and `Texture` constructors accept zero defaults and named fields; resource structs can be constructed empty. Nested fields such as `font.texture` refer to their parent struct, so in-place changes remain visible.

GPU resources, fonts, and raylib-owned images require their matching `unload_*` call. Keep their graphics context alive until unloading completes. Do not unload a resource twice or use aliases after its resource has been unloaded. Unloading an image clears that `Image` object's fields; this does not invalidate independent copies automatically.

`Image(buffer, width, height, 1, format)` borrows a contiguous Python buffer and retains its buffer export. The source object cannot resize while the image exists. Borrowed images cannot be passed to raylib image mutation or `unload_image`; release the Python image normally. Only one mipmap level is supported for borrowed images. `Image.to_bytes(size)` copies pixels and checks bounds. Images returned by `load_image` or `gen_image_color` are raylib-owned and can be modified and unloaded.

`update_texture` accepts contiguous Python buffers. Shader uniforms accept typed `array.array` buffers (`f`, `i`, or `I`, matching the uniform type), or `Vector2` where applicable. Buffer sizes and explicit element counts are checked before calls into raylib. Arguments are borrowed only for the duration of these calls.

`set_trace_log_callback` accepts a callable `(level, formatted_text)` or `None`. It retains the callable until replacement or removal; callers do not receive a raw `va_list`. The callback acquires the Python GIL. Blocking frame presentation releases the GIL so Python workers can run; longer image, font, texture, and resource operations also release it. Python argument conversion and callbacks run with the GIL. Follow raylib's graphics-thread requirements when using multiple Python threads.

## Tests

After installation, run the CPU-only semantic and ownership tests from the repository root:

```sh
python -m unittest discover -s python/tests -v
```

Enable the graphics and GIL regression with a working graphics backend:

```sh
EGL_PLATFORM=surfaceless RAYLIB_BACKEND=headless RAYLIB_TEST_RENDER=1 \
    python -m unittest discover -s python/tests -v
```
