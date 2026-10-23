#!/usr/bin/env python3
"""adb helpers for the Quest build: find the headset, install, launch, logcat, push the developer data."""
import subprocess
import sys
from pathlib import Path

ACTIVITY = "org.pagetable.diorama64/android.app.NativeActivity"
LOG_TAG = "Diorama64"
# The app's external files directory (Context.getExternalFilesDir(null), ANativeActivity::externalDataPath): adb can
# write it and the app reads it without a storage permission.
FILES_DIR = "/sdcard/Android/data/org.pagetable.diorama64/files"
RES_DIRS = ("lang", "dynos", "palettes", "mods")


def find_device(adb_devices_output: str) -> "str | None":
    """Serial of the single connected, authorized device; None for zero or several."""
    serials = []
    for line in adb_devices_output.splitlines()[1:]:
        parts = line.split()
        if len(parts) == 2 and parts[1] == "device":
            serials.append(parts[0])
    return serials[0] if len(serials) == 1 else None


def _serial() -> str:
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True, check=True).stdout
    serial = find_device(out)
    if serial is None:
        raise SystemExit("quest_device: need exactly one connected, authorized device (see `adb devices`)")
    return serial


def install(apk: Path) -> None:
    subprocess.run(["adb", "-s", _serial(), "install", "-r", "-d", str(apk)], check=True)


def launch() -> None:
    subprocess.run(["adb", "-s", _serial(), "shell", "am", "start", "-n", ACTIVITY], check=True)


def dev_data_plan(rom: Path, res_dir: Path, packs: "list[Path]") -> "list[tuple[Path, str]]":
    """(local source, remote path) for each push: the ROM, the four resource folders sm64coopdx reads from res/, and
    each pack folder under packs/<name>."""
    plan = [(rom, f"{FILES_DIR}/rom.z64")]
    plan += [(res_dir / d, f"{FILES_DIR}/res/{d}") for d in RES_DIRS]
    plan += [(p, f"{FILES_DIR}/packs/{p.name}") for p in packs]
    return plan


def push_dev_data(rom: Path, res_dir: Path, packs: "list[Path]") -> None:
    """Replaces each remote target whole (adb push of a folder onto an existing one would nest it), so files a previous
    push left behind never linger. packs/ is emptied first, so only the packs listed now are seen by the app."""
    plan = dev_data_plan(rom, res_dir, packs)
    missing = [str(src) for src, _ in plan if not src.exists()]
    if missing:
        raise SystemExit("quest_device: missing developer data: " + ", ".join(missing))
    serial = _serial()
    shell = ["adb", "-s", serial, "shell"]
    subprocess.run([*shell, "rm", "-rf", f"{FILES_DIR}/packs", f"{FILES_DIR}/rom.z64",
                    *(f"{FILES_DIR}/res/{d}" for d in RES_DIRS)], check=True)
    subprocess.run([*shell, "mkdir", "-p", f"{FILES_DIR}/res", f"{FILES_DIR}/packs"], check=True)
    for src, remote in plan:
        subprocess.run(["adb", "-s", serial, "push", str(src), remote], check=True)


def logcat(pattern: "str | None" = None) -> None:
    cmd = ["adb", "-s", _serial(), "logcat", "-s", LOG_TAG]
    if pattern is None:
        subprocess.run(cmd)
        return
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    try:
        for line in proc.stdout:
            if pattern in line:
                sys.stdout.write(line)
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "logcat":
        logcat(sys.argv[2] if len(sys.argv) > 2 else None)
    elif len(sys.argv) >= 3 and sys.argv[1] == "install":
        install(Path(sys.argv[2]))
    elif len(sys.argv) >= 2 and sys.argv[1] == "launch":
        launch()
    else:
        raise SystemExit("usage: quest_device.py logcat [pattern] | install <apk> | launch")
