#!/usr/bin/env python3
"""Configure, build, package (and optionally install/launch) the Quest APK."""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quest_bundle  # noqa: E402
import quest_device  # noqa: E402
import quest_package  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "quest"
ASSETS = BUILD / "assets"
STRIPPED = ROOT / "build" / "quest-stripped"   # outside BUILD, which collect_so searches
ROM = ROOT / "local" / "baserom.us.z64"


def strip_debug(sos: "list[Path]") -> "list[Path]":
    """Copies of the libraries without debug info (llvm-strip --strip-debug, from the NDK), for a Release APK."""
    tool = Path(os.environ["ANDROID_NDK_ROOT"]) / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
    shutil.rmtree(STRIPPED, ignore_errors=True)
    STRIPPED.mkdir(parents=True)
    out = []
    for so in sos:
        dst = STRIPPED / so.name
        shutil.copyfile(so, dst)
        subprocess.run([str(tool), "--strip-debug", str(dst)], check=True)
        print(f"strip {so.name}: {so.stat().st_size / 1e6:.1f} MB -> {dst.stat().st_size / 1e6:.1f} MB")
        out.append(dst)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", choices=["Debug", "Release"], default="Release")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--launch", action="store_true")
    ap.add_argument("--packs", help="the prepared packs to bundle in the APK, by name, comma-separated "
                    "(default: every prepared pack)")
    ap.add_argument("--no-packs", action="store_true", help="bundle no packs")
    args = ap.parse_args()
    if args.no_packs and args.packs is not None:
        ap.error("--packs and --no-packs exclude each other")
    names = [] if args.no_packs else None if args.packs is None else [n.strip() for n in args.packs.split(",") if n.strip()]
    if not ROM.is_file():
        raise SystemExit(f"quest_build: {ROM} missing (your own SM64 US ROM, as a .z64: python3 scripts/rom_to_z64.py)")
    env = os.environ
    # The upstream tree (patched), Lua and the host oracle's generated sources and resources, as visionos_build.py does.
    for script in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        subprocess.run([sys.executable, str(ROOT / "scripts" / script)], check=True)
    try:   # the ROM, the packs and the game's resources go INTO the APK as assets (the app copies them on first launch)
        quest_bundle.stage(ASSETS, names)
    except (ValueError, RuntimeError, OSError) as e:
        raise SystemExit(f"quest_build: the APK's data: {e}")
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
                    f"-DCMAKE_TOOLCHAIN_FILE={env['ANDROID_NDK_ROOT']}/build/cmake/android.toolchain.cmake",
                    "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-32", "-DANDROID_STL=c++_static",
                    f"-DCMAKE_BUILD_TYPE={args.config}", "-DSM64AR_SM64=ON",   # ON, not dropped: an older cache says OFF
                    f"-DSM64AR_OPENXR_DIR={env.get('SM64AR_OPENXR_DIR', '')}",
                    f"-DSM64AR_GLSLANG_DIR={env.get('SM64AR_GLSLANG_DIR', '')}"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD)], check=True)
    apk = BUILD / "Diorama64.apk"
    sos = quest_package.collect_so(BUILD)
    if args.config == "Release":
        sos = strip_debug(sos)
    quest_package.package(BUILD, sos, ASSETS, apk)
    print(f"built {apk} ({apk.stat().st_size / 1e6:.1f} MB)")
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
