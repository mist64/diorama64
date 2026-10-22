#!/usr/bin/env python3
"""Configure, build, package (and optionally install/launch) the Quest APK."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quest_device  # noqa: E402
import quest_package  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "quest"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", choices=["Debug", "Release"], default="Debug")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--launch", action="store_true")
    args = ap.parse_args()
    env = os.environ
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
                    f"-DCMAKE_TOOLCHAIN_FILE={env['ANDROID_NDK_ROOT']}/build/cmake/android.toolchain.cmake",
                    "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-32", "-DANDROID_STL=c++_static",
                    f"-DCMAKE_BUILD_TYPE={args.config}", "-DSM64AR_SM64=OFF",
                    f"-DSM64AR_OPENXR_DIR={env.get('SM64AR_OPENXR_DIR', '')}",
                    f"-DSM64AR_GLSLANG_DIR={env.get('SM64AR_GLSLANG_DIR', '')}"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD)], check=True)
    apk = BUILD / "Diorama64.apk"
    quest_package.package(BUILD, quest_package.collect_so(BUILD), None, apk)
    print(f"built {apk}")
    if args.no_install:
        return
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True, check=True).stdout
    if quest_device.find_device(out) is None:
        raise SystemExit("quest_build: no single authorized device connected; APK built but not installed "
                         "(use --no-install to skip this check)")
    quest_device.install(apk)
    if args.launch:
        quest_device.launch()


if __name__ == "__main__":
    main()
