#!/usr/bin/env python3
"""Stage the data the Quest APK carries as assets (the Vision Pro app bundles the same in its .app):

  <out>/Bundled/rom.z64, Bundled/packs/<name>/...   bundle_local.stage (ROM checked, packs re-verified and stamped)
  <out>/Bundled/bundled.json                        {"files": {"rom.z64": "<sha256>", "packs/<name>/<file>": ...}}
  <out>/res/{lang,dynos,palettes,mods}/...          the game's resources (sm64_oracle.py's build/_src/.../us_pc)
  <out>/res/res.json                                {"files": {"lang/English.ini": "<sha256>", ...}} (assets cannot
                                                    be listed recursively; the hashes make a changed resource file,
                                                    e.g. after an sm64 rebuild, change the manifest and so recopy)

The app copies them to its private files directory on first launch and whenever the manifests change.
"""
import json
import shutil
from pathlib import Path

import bundle_local

ROOT = Path(__file__).resolve().parents[1]
RES_DIR = ROOT / "build" / "_src" / "sm64" / "build" / "us_pc"
RES_DIRS = ("lang", "dynos", "palettes", "mods")


def stage(out_assets: Path, packs: "list[str] | None", res_dir: Path = RES_DIR) -> dict:
    """Returns the expected {relative path under Bundled/: sha256}. packs None: every prepared pack; []: none."""
    out_assets = Path(out_assets)
    if out_assets.exists():
        shutil.rmtree(out_assets)
    out_assets.mkdir(parents=True)
    expected = bundle_local.stage(out_assets / "Bundled", names=packs)
    bad = [k for k in expected if not k.isascii()]
    if bad:
        raise ValueError(f"non-ASCII bundled names (the app's manifest parser reads ASCII): {bad[:3]}")
    (out_assets / "Bundled" / "bundled.json").write_text(json.dumps({"files": expected}, indent=1) + "\n")
    missing = [d for d in RES_DIRS if not (Path(res_dir) / d).is_dir()]
    if missing:
        raise ValueError(f"{res_dir}: missing {', '.join(missing)} (python3 scripts/sm64_oracle.py)")
    files = {}
    for d in RES_DIRS:
        shutil.copytree(Path(res_dir) / d, out_assets / "res" / d)
    for p in sorted((out_assets / "res").rglob("*")):
        if p.is_file() and not bundle_local.finder_junk(p):
            files[p.relative_to(out_assets / "res").as_posix()] = bundle_local.sha256(p)
    (out_assets / "res" / "res.json").write_text(json.dumps({"files": files}, indent=1) + "\n")
    return expected
