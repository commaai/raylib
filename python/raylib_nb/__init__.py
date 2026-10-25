"""Direct nanobind bindings for the raylib UI API."""

from raylib_nb import enums as _enums
from raylib_nb._backend import native
from raylib_nb.colors import *  # noqa: F403
from raylib_nb.enums import *  # noqa: F403

# Export native functions and structs without adding Python wrappers on hot paths.
globals().update({name: getattr(native, name) for name in dir(native) if not name.startswith("_")})
for _enum in (
    _enums.ConfigFlags,
    _enums.TraceLogLevel,
    _enums.KeyboardKey,
    _enums.MouseButton,
    _enums.PixelFormat,
    _enums.TextureFilter,
    _enums.TextureWrap,
    _enums.ShaderUniformDataType,
):
    globals().update(_enum.__members__)
del _enum, _enums
