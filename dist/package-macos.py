#!/usr/bin/env python3
"""Bundle a macOS binary and its non-system dylibs with relative load paths."""

import pathlib
import shutil
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True)


def dependencies(path):
    return [line.strip().split(" (", 1)[0]
            for line in run("otool", "-L", str(path)).splitlines()[1:]]


binary, destination, r2_libdir = map(pathlib.Path, sys.argv[1:])
destination.mkdir(parents=True)
libdir = destination / "lib"
libdir.mkdir()
binary = pathlib.Path(shutil.copy2(binary, destination / "r2fuse"))
pending = [binary]
bundled = {}
while pending:
    target = pending.pop()
    for dependency in dependencies(target):
        if dependency.startswith(("/usr/lib/", "/System/")):
            continue
        name = pathlib.Path(dependency).name
        if target.parent == libdir and name == target.name:
            continue  # LC_ID_DYLIB, rather than a dependency
        if name not in bundled:
            source = r2_libdir / name
            if not source.is_file():
                source = pathlib.Path(dependency)
            if not source.is_file():
                raise RuntimeError(f"Cannot bundle {dependency}")
            library = libdir / name
            shutil.copy2(source, library)
            library.chmod(0o755)
            run("install_name_tool", "-id", "@loader_path/" + name, str(library))
            bundled[name] = library
            pending.append(library)
        relative = "@loader_path/lib/" if target == binary else "@loader_path/"
        run("install_name_tool", "-change", dependency, relative + name, str(target))

for target in [*bundled.values(), binary]:
    run("codesign", "--force", "--sign", "-", str(target))
    for dependency in dependencies(target):
        if not dependency.startswith(("/usr/lib/", "/System/", "@loader_path/")):
            raise RuntimeError(f"Nonportable dependency in {target}: {dependency}")
