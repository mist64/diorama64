#!/usr/bin/env python3
import os, sys
from sm64ar_common import BUILD_FLAGS, IS_MAC, ROOT, SRC, make_program, run, tool_problems

STAMP = SRC / "build" / ".ar-oracle"
EXE = SRC / "build" / "us_pc" / "sm64coopdx"


def up_to_date():
    prep = SRC / ".ar-stamp"
    return prep.is_file() and STAMP.is_file() and EXE.exists() and STAMP.read_text() == prep.read_text()


def main():
    if run([sys.executable, ROOT / "scripts" / "sm64_prepare.py"], check=False):
        return 1
    if up_to_date():
        print("sm64 desktop build up to date")
        return 0
    try:
        missing = tool_problems()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if missing and IS_MAC:
        print(f"error: missing Homebrew formulae: {' '.join(missing)} (install them: brew install {' '.join(missing)})",
              file=sys.stderr)
        return 1
    if missing:
        print(f"error: missing tools: {' '.join(missing)} (run inside the Nix dev shell: nix develop)", file=sys.stderr)
        return 1
    STAMP.unlink(missing_ok=True)
    # The sm64coopdx Makefile assumes Homebrew gcc (a versioned cpp-N) and coreutils (gcp) on macOS.
    # Use Xcode's clang instead; -x c because the inputs (level_rules.mk, ...) are not .c files. The Linux dev shell is
    # clang-only too (no cpp binary), so the same flags apply there. macOS keeps Homebrew's gmake by name: env() puts
    # Homebrew on PATH for the child, which this process's PATH may not have.
    # On Linux CC/CXX too: the Makefiles name gcc/g++ (on macOS those are Xcode's clang shims) and the shell has neither.
    # Command-line variables reach the tools/ sub-make as well.
    host = [] if IS_MAC else ["CC=clang", "CXX=clang++"]
    run(["gmake" if IS_MAC else make_program(), "-C", SRC, f"-j{os.cpu_count()}", *BUILD_FLAGS, "CPP=clang -E -P -x c", "CP=cp",
         *host])
    # On macOS the game resolves resources (lang, mods, palettes, dynos, ...) at <exe dir>/../Resources
    # (src/pc/platform.c sys_resource_path, app-bundle layout). Point that at the plain build dir.
    res = SRC / "build" / "Resources"
    if IS_MAC and not res.is_symlink():
        res.symlink_to("us_pc")
    if not EXE.exists():
        print("MISSING oracle executable", file=sys.stderr)
        return 1
    STAMP.write_text((SRC / ".ar-stamp").read_text())
    print(EXE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
