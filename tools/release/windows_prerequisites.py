#!/usr/bin/env python3
# Stage the Windows build tools next to the installer before the release is
# zipped. Portable tools travel with source/ so updates keep their paths.
# The remaining packages are original, unmodified vendor installers.

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


# Pin the x64 assets and SHA-256 values published by the vendors or recorded
# in the matching winget manifests. A changed download must fail packaging.
PACKAGES = (
    ("cmake.zip",
     "https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip",
     "81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719"),
    ("ninja.zip",
     "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"),
    ("ninja-LICENSE.txt",
     "https://raw.githubusercontent.com/ninja-build/ninja/v1.13.2/COPYING",
     "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea"),
    ("llvm.exe",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-win64.exe",
     "7a5386c26497db1691f320121e5b113364dd0274b98e55f15f4dbc00c0450113"),
    ("python.exe",
     "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe",
     "67b5635e80ea51072b87941312d00ec8927c4db9ba18938f7ad2d27b328b95fb"),
    ("git.exe",
     "https://github.com/git-for-windows/git/releases/download/v2.53.0.windows.1/Git-2.53.0-64-bit.exe",
     "3b4e1b127dbebea2931f2ae9dfafa0c2343a488a1222009debfe78d5d335e6a9"),
    ("vulkan-sdk.exe",
     "https://sdk.lunarg.com/sdk/download/1.4.341.1/windows/vulkansdk-windows-X64-1.4.341.1.exe",
     "bcf2d75aa9556889ab974858666e20b3655b6055a0db704ccb47279ff33b5bfe"),
    ("vc-redist.exe",
     "https://download.visualstudio.microsoft.com/download/pr/6f02464a-5e9b-486d-a506-c99a17db9a83/"
     "8995548DFFFCDE7C49987029C764355612BA6850EE09A7B6F0FDDC85BDC5C280/VC_redist.x64.exe",
     "8995548dfffcde7c49987029c764355612ba6850ee09a7b6f0fddc85bdc5c280"),
)


def sha256(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def stage_packages(stage, layout, cache=None):
    portable = stage / "source" / "build-tools"
    installers = stage / "prerequisites"
    portable.mkdir(parents=True)
    installers.mkdir()
    # A Windows checkout can convert the port's patch files to CRLF even
    # though the SDK checkout uses LF. Git apply needs matching line endings.
    for patch in (stage / "source" / "patches" / "rexglue-sdk").glob("*.patch"):
        patch.write_bytes(patch.read_bytes().replace(b"\r\n", b"\n"))
    offline_layout = installers / "vs-layout"
    shutil.copytree(layout, offline_layout)
    response = offline_layout / "Response.json"
    settings = json.loads(response.read_text(encoding="utf-8-sig"))
    # The stock layout uses an online update channel even with --noWeb.
    settings["channelUri"] = r".\ChannelManifest.json"
    response.write_text(json.dumps(settings) + "\n", encoding="utf-8")
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        for name, url, expected in PACKAGES:
            path = work / name
            print(f"   {name}", flush=True)
            cached = cache / name if cache else None
            if cached and cached.is_file():
                shutil.copyfile(cached, path)
            else:
                subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                                "--connect-timeout", "30", "--speed-time", "60", "--speed-limit", "1024",
                                "--retry", "3", "--output", str(path), url], check=True)
            actual = sha256(path)
            if actual != expected:
                raise SystemExit(f"{name}: SHA-256 {actual}, expected {expected}")

            if name == "cmake.zip":
                with zipfile.ZipFile(path) as archive:
                    archive.extractall(work)
                shutil.move(work / "cmake-3.31.8-windows-x86_64", portable / "cmake")
            elif name == "ninja.zip":
                with zipfile.ZipFile(path) as archive:
                    archive.extract("ninja.exe", portable)
            else:
                shutil.move(path, installers if path.suffix == ".exe" else portable)


if __name__ == "__main__":
    # A release builder can reuse previously downloaded vendor archives.
    # Cached packages go through the same hash check as fresh downloads.
    cache = os.environ.get("MOR_WINDOWS_PACKAGE_CACHE")
    stage_packages(Path(sys.argv[1]), Path(sys.argv[2]), Path(cache) if cache else None)
