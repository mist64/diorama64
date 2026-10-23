#!/usr/bin/env python3
"""Build and run the Quest app on the desktop, on Monado's simulated headset (no Quest needed).

Run it inside the dev shell (direnv allow, or nix develop):

    python3 scripts/desktop_xr.py [--seconds N] [--no-build] [-- app arguments, e.g. --game sm64 --start story]

It configures build/desktop (SM64AR_DESKTOP_XR=ON), reuses the data quest_build.py staged (build/quest/assets: the
ROM, packs and resources), starts monado-service with the qwerty driver and its debug window unless one is already
running, and runs the app until you press Ctrl-C (or for --seconds). The app's log is printed and kept in
build/desktop/run/sm64ar.log; prefs.json, placement.json and saves live in build/desktop/run too.

Monado has no passthrough and no room scan, so the level stands on a fixed table 1 m in front of the start position.
The controls are Monado's qwerty driver, typed into its "Monado debug GUI" window (focus it first):
  head            WASD move, Q/E down/up, arrows or right-mouse-drag look
  right hand      (default) left mouse = trigger, middle mouse = grip, M = A (jump), N = B (punch), V = Y (Start), TFGH = stick
  left hand       hold Ctrl with the same keys: TFGH = stick (Mario moves), M = X, N = menu (the panel)
  either hand     hold Alt for the right hand explicitly; C toggles the hands following the head, R resets them
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "desktop"
RUN = BUILD / "run"
DATA = ROOT / "build" / "quest" / "assets"
APP = BUILD / "apps" / "desktop_xr" / "diorama64_desktop"


def monado_socket() -> Path:
    return Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "monado_comp_ipc"


def env_for_gpu() -> dict:
    """The dev shell points Vulkan at lavapipe (for the host tests); Monado and the app want the real GPU."""
    env = dict(os.environ)
    env.pop("VK_DRIVER_FILES", None)
    env.pop("VK_ICD_FILENAMES", None)
    return env


def build() -> None:
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DSM64AR_DESKTOP_XR=ON", "-DSM64AR_HOST_TESTS=OFF", "-DSM64AR_SM64=ON",
                    f"-DSM64AR_OPENXR_DIR={os.environ.get('SM64AR_OPENXR_DIR', '')}",
                    f"-DSM64AR_GLSLANG_DIR={os.environ.get('SM64AR_GLSLANG_DIR', '')}"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD), "--target", "diorama64_desktop"], check=True)


def start_monado(log: Path) -> "subprocess.Popen | None":
    if monado_socket().exists():
        if subprocess.run(["pgrep", "-x", "monado-service"], capture_output=True).returncode == 0:
            print(f"desktop_xr: using the running Monado ({monado_socket()})")
            return None
        monado_socket().unlink()   # a stale socket: Monado leaves it behind when it exits
    env = env_for_gpu()
    env.update({"XRT_DEBUG_GUI": "1", "QWERTY_ENABLE": "1"})
    # monado-service watches stdin for its console commands; it refuses a stdin it cannot poll (/dev/null).
    proc = subprocess.Popen(["monado-service"], env=env, stdin=subprocess.PIPE, stdout=log.open("w"), stderr=subprocess.STDOUT)
    for _ in range(100):
        if monado_socket().exists():
            print(f"desktop_xr: Monado started (log {log})")
            return proc
        if proc.poll() is not None:
            raise SystemExit(f"desktop_xr: monado-service exited ({proc.returncode}); see {log}")
        time.sleep(0.1)
    proc.terminate()
    raise SystemExit(f"desktop_xr: monado-service did not open {monado_socket()} in 10 s; see {log}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-build", action="store_true", help="run the existing build")
    ap.add_argument("--seconds", type=float, help="quit (as Ctrl-C does) after this many seconds")
    ap.add_argument("app_args", nargs="*", help="the app's own arguments (after --); default: --game sm64")
    args = ap.parse_args()
    for var in ("SM64AR_OPENXR_DIR", "SM64AR_GLSLANG_DIR", "SM64AR_MONADO_JSON"):
        if not os.environ.get(var):
            raise SystemExit(f"desktop_xr: {var} unset; run inside the dev shell (direnv allow, or nix develop)")
    if not (DATA / "Bundled" / "rom.z64").is_file() or not (DATA / "res" / "lang" / "English.ini").is_file():
        raise SystemExit(f"desktop_xr: no staged data in {DATA}; run ./build.py (or scripts/quest_build.py --no-install) once")
    if not args.no_build:
        build()
    RUN.mkdir(parents=True, exist_ok=True)
    monado = start_monado(RUN / "monado.log")
    env = env_for_gpu()
    env["XR_RUNTIME_JSON"] = os.environ["SM64AR_MONADO_JSON"]
    cmd = [str(APP), "--bundle", str(DATA), "--docs", str(RUN)] + (args.app_args or ["--game", "sm64"])
    app = subprocess.Popen(cmd, env=env)
    try:
        if args.seconds is None:
            app.wait()
        else:
            try:
                app.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                app.send_signal(signal.SIGINT)
                app.wait(timeout=15)
    except KeyboardInterrupt:   # Ctrl-C reached the app too (same process group); let it shut down
        app.wait(timeout=15)
    finally:
        if monado is not None:
            monado.terminate()
            try:
                monado.wait(timeout=5)
            except subprocess.TimeoutExpired:
                monado.kill()
    sys.exit(app.returncode if app.returncode and app.returncode > 0 else 0)


if __name__ == "__main__":
    main()
