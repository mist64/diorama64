#!/usr/bin/env python3
"""adb helpers for the Quest build: find the headset, install, launch, logcat."""
import subprocess
import sys
from pathlib import Path

ACTIVITY = "org.pagetable.diorama64/android.app.NativeActivity"
LOG_TAG = "Diorama64"


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
