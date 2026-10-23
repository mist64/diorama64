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
RES_DIR = ROOT / "build" / "_src" / "sm64" / "build" / "us_pc"   # lang/ dynos/ palettes/ mods/ (sm64_oracle.py)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", choices=["Debug", "Release"], default="Debug")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--launch", action="store_true")
    ap.add_argument("--rom", type=Path, help="after the install, push this .z64 ROM, the game's resources and the "
                    "--packs to the app's external files directory")
    ap.add_argument("--packs", default="", help="with --rom: the prepared packs to push, by name, comma-separated "
                    "(default: none)")
    args = ap.parse_args()
    names = [n.strip() for n in args.packs.split(",") if n.strip()]
    if names and args.rom is None:
        ap.error("--packs needs --rom")
    if args.rom is not None and args.no_install:
        ap.error("--rom pushes after the install; it cannot go with --no-install")
    env = os.environ
    # The upstream tree (patched), Lua and the host oracle's generated sources and resources, as visionos_build.py does.
    for script in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        subprocess.run([sys.executable, str(ROOT / "scripts" / script)], check=True)
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
                    f"-DCMAKE_TOOLCHAIN_FILE={env['ANDROID_NDK_ROOT']}/build/cmake/android.toolchain.cmake",
                    "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-32", "-DANDROID_STL=c++_static",
                    f"-DCMAKE_BUILD_TYPE={args.config}", "-DSM64AR_SM64=ON",   # ON, not dropped: an older cache says OFF
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
    if args.rom is not None:
        push_data(args.rom, names)
    if args.launch:
        quest_device.launch()


def push_data(rom: Path, names: "list[str]") -> None:
    """Staged as visionos_build.py bundles them: the ROM checked, each named pack re-verified and stamped (the app
    requires the stamp), then pushed."""
    import bundle_local
    staged = BUILD / "dev-data"
    try:
        bundle_local.stage(staged, rom=rom.resolve(), names=names)
    except (ValueError, RuntimeError, OSError) as e:
        raise SystemExit(f"quest_build: the developer data: {e}")
    quest_device.push_dev_data(staged / "rom.z64", RES_DIR, [staged / "packs" / n for n in names])


if __name__ == "__main__":
    main()
