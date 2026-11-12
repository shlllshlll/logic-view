#!/usr/bin/env python3
"""Make a macOS .app self-contained.

macdeployqt handles Qt frameworks/plugins and most Homebrew dylibs, but it
misses some indirect dependencies (for example libbrotlicommon) and it cannot
deploy Homebrew's Python framework.  This script:

  * copies every remaining /opt/homebrew dylib into Contents/Frameworks,
  * rewrites dependencies to @executable_path/../Frameworks/...,
  * drops build-machine rpaths,
  * signs every Mach-O file (macOS kills arm64 processes that load an
    unsigned/edited .so with "Code Signature Invalid").

Run from macos/bundle.sh, not directly.
"""

import glob
import os
import plistlib
import re
import shutil
import subprocess
import sys
from collections import deque

APP = os.path.abspath(sys.argv[1])
FW = os.path.join(APP, "Contents", "Frameworks")
MAIN = os.path.join(APP, "Contents", "MacOS", "LogicAnalyzer")
HB = os.environ.get("HOMEBREW_PREFIX", "/opt/homebrew")
find_cache = {}


def is_macho(path):
    try:
        with open(path, "rb") as fh:
            magic = fh.read(4)
        return magic in (
            b"\xcf\xfa\xed\xfe",  # 64-bit LE
            b"\xce\xfa\xed\xfe",  # 32-bit LE
            b"\xca\xfe\xba\xbe",  # fat
            b"\xbe\xba\xfe\xca",
        )
    except OSError:
        return False


def macho_files():
    roots = [
        os.path.join(APP, "Contents", "MacOS"),
        FW,
        os.path.join(APP, "Contents", "PlugIns"),
    ]
    for root in roots:
        for dirpath, _, names in os.walk(root):
            for name in names:
                path = os.path.join(dirpath, name)
                if not os.path.islink(path) and is_macho(path):
                    yield path


def otool_l(path):
    try:
        return subprocess.run(
            ["otool", "-L", path], capture_output=True, text=True
        ).stdout
    except OSError:
        return ""


def dylib_id(path):
    """Return the LC_ID_DYLIB name (plugins and dylibs have one; bundles
    and executables usually do not).  otool -L lists it as a dependency."""
    try:
        out = subprocess.run(
            ["otool", "-D", path], capture_output=True, text=True
        ).stdout
    except OSError:
        return None
    lines = [line.strip() for line in out.splitlines()[1:] if line.strip()]
    return lines[0] if lines else None


def deps(path):
    ident = dylib_id(path)
    result = []
    for line in otool_l(path).splitlines()[1:]:
        dep = line.strip().split(" ")[0]
        if dep and dep != ident:
            result.append(dep)
    return result


def find_source(name, hint=None):
    if hint and os.path.exists(hint):
        return os.path.realpath(hint)
    if name in find_cache:
        return find_cache[name]
    candidates = glob.glob(os.path.join(HB, "opt", "*", "lib", name))
    candidates += glob.glob(os.path.join(HB, "lib", name))
    candidates += glob.glob(
        os.path.join(HB, "opt", "*", "Frameworks", "*.framework", "Versions", "*", name)
    )
    result = os.path.realpath(candidates[0]) if candidates else None
    find_cache[name] = result
    return result


def copy_lib(src, name, queue):
    dst = os.path.join(FW, name)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
        print("  +", name, flush=True)
        queue.append(dst)
    subprocess.run(
        ["install_name_tool", "-id", "@executable_path/../Frameworks/" + name, dst],
        capture_output=True,
    )
    return dst


def bundle_id(path):
    return "@executable_path/../Frameworks/" + os.path.relpath(path, FW)


def main():
    if not os.path.isdir(FW):
        print("not an app bundle:", APP, file=sys.stderr)
        return 1

    info_path = os.path.join(APP, "Contents", "Info.plist")
    try:
        with open(info_path, "rb") as fh:
            info = plistlib.load(fh)
    except (OSError, plistlib.InvalidFileException) as exc:
        print("invalid Info.plist:", exc, file=sys.stderr)
        return 1
    icon = info.get("CFBundleIconFile")
    if icon and not os.path.exists(os.path.join(APP, "Contents", "Resources", icon)):
        print("missing bundle icon:", icon, file=sys.stderr)
        return 1

    # Library IDs still pointing at the build machine or Homebrew.
    for path in list(macho_files()):
        if path == MAIN or path.endswith(".so") or "/PlugIns/" in path:
            continue
        first = otool_l(path).splitlines()
        if first and first[0].rstrip(":").strip().startswith("/"):
            subprocess.run(
                ["install_name_tool", "-id", bundle_id(path), path],
                capture_output=True,
            )

    # Copy missing dependencies and rewrite every reference.
    queue = deque(macho_files())
    seen = set()
    while queue:
        path = queue.popleft()
        if path in seen:
            continue
        seen.add(path)
        for dep in deps(path):
            new = None
            if dep.startswith(HB + "/"):
                if "/Python.framework/" in dep:
                    new = (
                        "@executable_path/../Frameworks/Python.framework/"
                        + dep.split("/Python.framework/", 1)[1]
                    )
                else:
                    name = os.path.basename(dep)
                    src = find_source(name, dep)
                    if not src:
                        print("missing:", dep, "in", path, file=sys.stderr)
                        continue
                    copy_lib(src, name, queue)
                    new = "@executable_path/../Frameworks/" + name
            elif dep.startswith("@rpath/"):
                rest = dep[len("@rpath/"):]
                if ".framework/" in rest and os.path.exists(os.path.join(FW, rest)):
                    continue
                name = os.path.basename(rest)
                if not os.path.exists(os.path.join(FW, name)):
                    src = find_source(name)
                    if not src:
                        print("missing:", dep, "in", path, file=sys.stderr)
                        continue
                    copy_lib(src, name, queue)
                new = "@executable_path/../Frameworks/" + name
            elif dep.startswith("@executable_path/../Frameworks/"):
                rest = dep[len("@executable_path/../Frameworks/"):]
                if os.path.exists(os.path.join(FW, rest)):
                    continue
                name = os.path.basename(rest)
                src = find_source(name)
                if not src:
                    print("missing:", dep, "in", path, file=sys.stderr)
                    continue
                copy_lib(src, name, queue)
                new = "@executable_path/../Frameworks/" + name
            if new and new != dep:
                subprocess.run(
                    ["install_name_tool", "-change", dep, new, path],
                    capture_output=True,
                )

    # Remove absolute rpaths; they are build-machine specific.
    for path in list(macho_files()):
        out = subprocess.run(
            ["otool", "-l", path], capture_output=True, text=True
        ).stdout
        for line in out.splitlines():
            match = re.search(r"path (\S+) \(offset", line)
            if match and (
                match.group(1).startswith(HB)
                or "/logic-view/install/lib" in match.group(1)
            ):
                subprocess.run(
                    ["install_name_tool", "-delete_rpath", match.group(1), path],
                    capture_output=True,
                )

    # Sign every Mach-O file, including lib-dynload/*.so.  macOS kills a
    # process when it loads an edited/unsigned .so on arm64.
    failures = 0
    for path in macho_files():
        if path == MAIN:
            continue
        result = subprocess.run(
            ["codesign", "--force", "--sign", "-", path],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            failures += 1
            print("codesign failed:", path, result.stderr.strip(), file=sys.stderr)

    result = subprocess.run(
        ["codesign", "--force", "--deep", "--sign", "-", APP],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        failures += 1
        print("codesign app failed:", result.stderr.strip(), file=sys.stderr)

    # Report unresolved absolute dependencies (IDs are intentionally not
    # checked: Qt plugin IDs may remain their Homebrew path and are harmless).
    bad = 0
    for path in macho_files():
        for dep in deps(path):
            if dep.startswith(HB) or "/logic-view/install" in dep:
                print("leftover:", dep, "in", path, file=sys.stderr)
                bad += 1
    print("  dependencies:", "ok" if bad == 0 else f"{bad} leftover", flush=True)
    return 1 if (bad or failures) else 0


if __name__ == "__main__":
    sys.exit(main())
