import importlib
import os

backend = os.getenv("RAYLIB_BACKEND", "").strip().lower()
if not backend:
    backend = "comma" if os.path.exists("/AGNOS") or os.path.exists("/TICI") else "desktop"
if backend not in ("desktop", "comma", "headless"):
    raise ValueError(f"invalid RAYLIB_BACKEND: {backend!r}")
native = importlib.import_module(f"{__package__}._raylib_{backend}")
