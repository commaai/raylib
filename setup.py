import os
import platform
import shlex
import shutil
import subprocess
import sysconfig
import tempfile
from pathlib import Path

import nanobind
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext
from setuptools.errors import PlatformError, SetupError

ROOT = Path(__file__).resolve().parent
SYSTEM = platform.system()
if SYSTEM not in ("Linux", "Darwin"):
    raise PlatformError("The Python bindings currently support Linux and macOS")

if Path("/AGNOS").exists():
    os.environ.setdefault("CC", "clang")
    os.environ.setdefault("CXX", "clang++")


def backends():
    defaults = ["desktop"] if SYSTEM == "Darwin" else ["desktop", "headless"]
    if SYSTEM == "Linux" and platform.machine() in ("aarch64", "arm64"):
        defaults.append("comma")
    selected = os.getenv("RAYLIB_BACKENDS", ",".join(defaults)).split(",")
    selected = [value.strip() for value in selected]
    allowed = {"desktop"} if SYSTEM == "Darwin" else {"desktop", "headless", "comma"}
    if not selected or any(value not in allowed for value in selected) or len(set(selected)) != len(selected):
        raise SetupError(f"RAYLIB_BACKENDS must be a comma-separated selection of {sorted(allowed)}")
    return selected


def libraries(backend):
    if SYSTEM == "Darwin":
        return []
    if backend == "desktop":
        return ["GL", "X11", "m", "pthread", "dl", "rt"]
    return ["GLESv2", "EGL", "m", "pthread", "dl", "rt"] + (["gbm", "drm"] if backend == "comma" else [])


class BuildBindings(build_ext):
    def build_extensions(self):
        # Each extension uses a different module macro. Keep both its raylib build
        # and C++ objects separate, even when build_ext --parallel is requested.
        self.parallel = None
        self.force = True
        self.compiler.force = True
        self.backend_build_root = Path(self.build_temp).resolve()
        super().build_extensions()

    def build_extension(self, extension):
        backend = extension.name.rsplit("_", 1)[-1]
        directory = self.backend_build_root / backend
        directory.mkdir(parents=True, exist_ok=True)
        jobs = os.getenv("RAYLIB_BUILD_JOBS", str(min(4, os.cpu_count() or 1)))
        if not jobs.isdecimal() or int(jobs) < 1:
            raise SetupError("RAYLIB_BUILD_JOBS must be a positive integer")

        # Use raylib's own build configuration without modifying src/ or relying on
        # a separately installed raylib package. A fresh copy prevents stale objects
        # from another backend or compiler configuration entering the archive.
        with tempfile.TemporaryDirectory(prefix="raylib-", dir=directory) as temporary:
            source = Path(temporary) / "src"
            shutil.copytree(
                ROOT / "src", source, ignore=shutil.ignore_patterns("*.o", "*.a", "*.so", "*.dylib", "*.dll")
            )
            target = {"desktop": "PLATFORM_DESKTOP", "headless": "PLATFORM_HEADLESS", "comma": "PLATFORM_COMMA"}[
                backend
            ]
            flags = ["-fPIC", "-fvisibility=hidden"]
            if SYSTEM == "Darwin":
                # Match the extension's architecture slices, including universal2 Python.
                configured = shlex.split(sysconfig.get_config_var("CFLAGS") or "")
                arch_flags = [
                    flag
                    for i, word in enumerate(configured[:-1])
                    if word == "-arch"
                    for flag in [word, configured[i + 1]]
                ]
                flags += shlex.split(os.getenv("ARCHFLAGS", shlex.join(arch_flags)))
                deployment = os.getenv("MACOSX_DEPLOYMENT_TARGET") or sysconfig.get_config_var(
                    "MACOSX_DEPLOYMENT_TARGET"
                )
                if deployment:
                    flags.append(f"-mmacosx-version-min={deployment}")
            command = [
                "make",
                f"-j{jobs}",
                f"PLATFORM={target}",
                f"CC={os.getenv('CC', 'cc')}",
                f"CUSTOM_CFLAGS={shlex.join(flags)}",
            ]
            archive = directory / f"libraylib_{backend}.a"
            if SYSTEM == "Darwin":
                objects = ["rcore.o", "rshapes.o", "rtextures.o", "rtext.o", "rmodels.o", "raudio.o", "rglfw.o"]
                subprocess.run(command + objects, cwd=source, check=True)
                subprocess.run(["libtool", "-static", "-o", str(archive), *objects], cwd=source, check=True)
            else:
                subprocess.run(command, cwd=source, check=True)
                shutil.copy2(source / "libraylib.a", archive)

        runtime = self.compiler.compile(
            [str(nb / "src" / "nb_combined.cpp")],
            output_dir=str(directory),
            include_dirs=extension.include_dirs + [sysconfig.get_path("include")],
            extra_postargs=extension.extra_compile_args,
        )
        extension.extra_objects = [str(archive), *runtime]
        previous = self.build_temp
        self.build_temp = str(directory)
        try:
            super().build_extension(extension)
        finally:
            self.build_temp = previous


nb = Path(nanobind.__file__).resolve().parent
frameworks = ["OpenGL", "Cocoa", "IOKit", "CoreAudio", "CoreVideo"] if SYSTEM == "Darwin" else []
extensions = [
    Extension(
        f"raylib_nb._raylib_{backend}",
        sources=["python/bindings.cc"],
        include_dirs=[str(ROOT / "src"), str(nb / "include"), str(nb / "ext" / "robin_map" / "include")],
        define_macros=[("RAYLIB_MODULE", f"_raylib_{backend}")],
        libraries=libraries(backend),
        extra_compile_args=["-std=c++17", "-fvisibility=hidden"],
        extra_link_args=[flag for framework in frameworks for flag in ["-framework", framework]],
        depends=["python/functions.h", "src/raylib.h", "src/rlgl.h", "src/raymath.h"],
        language="c++",
    )
    for backend in backends()
]

setup(ext_modules=extensions, cmdclass={"build_ext": BuildBindings})
