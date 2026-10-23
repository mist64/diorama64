"""sm64ar_common.py — shared paths and helpers for all tooling scripts."""
import functools, os, pathlib, plistlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
IS_MAC = sys.platform == "darwin"
PIN = "8cd6e5977d9f920d51ca71f2c61801d019ed79c6"
UPSTREAM = ROOT / "games" / "sm64" / "upstream"
# Overridable so tests can prepare a throwaway copy without touching the live worktree.
SRC = pathlib.Path(os.environ.get("SM64AR_SRC") or ROOT / "build" / "_src" / "sm64")
PATCH_DIR = pathlib.Path(os.environ.get("SM64AR_PATCH_DIR") or ROOT / "games" / "sm64" / "patches")
KEEP = pathlib.Path(os.environ.get("SM64AR_KEEP") or SRC.parent / "sm64-build-keep")
MIN_XCODE, TESTED_XCODE = 26, "26.3"
BUILD_FLAGS = ["DISCORD_SDK=0", "COOPNET=0", "UPDATER=0", "USE_APP=0"]
# Homebrew formulae the build needs (make: GNU make, since upstream's Makefile needs more than macOS's make 3.81).
FORMULAE = ("cmake", "ninja", "make", "sdl2", "glew")
# Linux (the Nix dev shell): the programs on PATH and the pkg-config modules the oracle build and pack prep need.
LINUX_TOOLS = ("cmake", "ninja", "make", "pkg-config", "magick", "bsdtar", "adb")
LINUX_PKGS = ("sdl2", "glew")
HARNESS = ROOT / "build" / "macos" / "harness" / "bin" / "sm64ar_harness"
PACK_TOOL = ROOT / "build" / "host" / "pack_tool" / "sm64ar_pack_tool"
RES_DIR = SRC / "build" / "us_pc"


def _selected_xcode():
    """`xcode-select -p` (read only: the build never changes which Xcode is selected)."""
    try:
        r = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True)
    except OSError:
        return ""
    return r.stdout.strip() if r.returncode == 0 else ""


def developer_folder(path):
    if path and (pathlib.Path(path) / "Contents" / "Developer").is_dir():
        return str(pathlib.Path(path) / "Contents" / "Developer")
    return path


DEVELOPER_DIR = developer_folder(os.environ.get("DEVELOPER_DIR") or (_selected_xcode() if IS_MAC else ""))
BREW_PLACES = ("/opt/homebrew/bin/brew", "/usr/local/bin/brew")


def find_brew(which=shutil.which, places=BREW_PLACES):
    return which("brew") or next((p for p in places if os.access(p, os.X_OK)), None)


def xcode_version(dev):
    try:
        return str(plistlib.loads((pathlib.Path(dev).parent / "version.plist").read_bytes()).get("CFBundleShortVersionString", ""))
    except (OSError, ValueError, plistlib.InvalidFileException):
        return ""


def has_xros_sdk(dev):
    return any((pathlib.Path(dev) / "Platforms" / "XROS.platform" / "Developer" / "SDKs").glob("XROS*.sdk"))


def xcode_check(version, xros_sdk, dev):
    m = re.match(r"(\d+)", version or "")
    if not m:
        return (f"no Xcode at {dev or '(none selected)'}: install Xcode {MIN_XCODE} or newer, then set "
                "DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer (or select it in Xcode > Settings > Locations)"), None
    if int(m.group(1)) < MIN_XCODE:
        return (f"Xcode {version} at {dev} is too old: install Xcode {MIN_XCODE} or newer and set DEVELOPER_DIR to its "
                "Contents/Developer"), None
    if not xros_sdk:
        return f"Xcode {version} at {dev} has no visionOS SDK: install it in Xcode > Settings > Components", None
    return None, (None if version == TESTED_XCODE else f"note: building with Xcode {version} (tested with Xcode {TESTED_XCODE})")


def xcode_problem(dev=None):
    dev = dev or DEVELOPER_DIR
    return xcode_check(xcode_version(dev) if dev else "", bool(dev) and has_xros_sdk(dev), dev)


@functools.lru_cache(maxsize=None)
def require_xcode():
    problem, note = xcode_problem()
    if problem:
        raise SystemExit(f"error: {problem}")
    if note:
        print(note, file=sys.stderr)
    return DEVELOPER_DIR


def env():
    e = dict(os.environ)
    if not IS_MAC:
        return e
    e["DEVELOPER_DIR"] = require_xcode()
    brew = find_brew()
    if brew and str(pathlib.Path(brew).parent) not in e.get("PATH", "").split(":"):
        e["PATH"] = f"{pathlib.Path(brew).parent}:{e.get('PATH', '')}"
    return e

def run(cmd, capture=False, cwd=None, check=True):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env(), check=check, text=True, capture_output=capture)
    return r.stdout if capture else r.returncode

def missing_formulae(names=FORMULAE, brew=None):
    brew = brew or find_brew()
    if not brew or not shutil.which(brew):
        raise RuntimeError("Homebrew is not installed: install it as https://brew.sh says")

    def listed(*n):
        return subprocess.run([brew, "list", "--versions", *n], capture_output=True).returncode == 0
    return [] if listed(*names) else [n for n in names if not listed(n)]


def tool_problems():
    """What the host lacks for the oracle build and pack prep. macOS: the missing Homebrew formulae (RuntimeError without
    Homebrew). Linux: the programs missing from PATH and the pkg-config modules pkg-config cannot find."""
    if IS_MAC:
        return missing_formulae()
    missing = [n for n in LINUX_TOOLS if not shutil.which(n)]
    if shutil.which("pkg-config"):
        missing += [m for m in LINUX_PKGS if subprocess.run(["pkg-config", "--exists", m]).returncode]
    else:
        missing += list(LINUX_PKGS)
    return missing


def make_program():
    """GNU make: Homebrew installs it as gmake (macOS's own make is 3.81); on Linux make is GNU make."""
    return "gmake" if shutil.which("gmake") else "make"


def pack_generator_cmd(rom, scratch):
    """The command that boots the game headlessly for DynOS's generators (the caller appends --generate-tex/-actors)."""
    if IS_MAC:
        return [str(HARNESS), "--offscreen", "--out", str(scratch), "--rom", str(rom)]
    return [str(PACK_TOOL), "--rom", str(rom), "--res", str(RES_DIR)]


def resize_png_cmd(src, dst, cap):
    """Re-encode src as a PNG at dst, scaled down to fit cap x cap when cap is given."""
    if IS_MAC:
        return (["/usr/bin/sips", "-Z", str(cap), str(src), "--out", str(dst)] if cap
                else ["/usr/bin/sips", "-s", "format", "png", str(src), "--out", str(dst)])
    return ["magick", f"png:{src}", "-resize", f"{cap}x{cap}>", f"png:{dst}"] if cap else ["magick", f"png:{src}", f"png:{dst}"]
