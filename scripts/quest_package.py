#!/usr/bin/env python3
"""Package native libraries into a signed, aligned Quest APK."""
import os
import subprocess
import zipfile
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[1] / "apps" / "quest" / "AndroidManifest.xml"


def aapt2_link_cmd(aapt2, manifest, android_jar, assets_dir, out) -> list:
    cmd = [str(aapt2), "link", "-o", str(out), "--manifest", str(manifest),
           "-I", str(android_jar), "--min-sdk-version", "32", "--target-sdk-version", "32"]
    if assets_dir is not None:
        cmd += ["-A", str(assets_dir)]
    return cmd


def missing_assets(manifest_paths: "list[str]", zip_names: "list[str]") -> "list[str]":
    """The listed asset paths (relative to assets/) absent from the zip."""
    have = set(zip_names)
    return [p for p in manifest_paths if "assets/" + p not in have]


def listed_assets(assets_dir: Path) -> "list[str]":
    """Every path the app will open: the two manifests and the files they list (relative to assets/)."""
    import json
    assets_dir = Path(assets_dir)
    out = []
    for manifest, prefix in (("Bundled/bundled.json", "Bundled/"), ("res/res.json", "res/")):
        f = assets_dir / manifest
        if not f.is_file():
            continue
        data = json.loads(f.read_text())
        names = data["files"] if isinstance(data, dict) else data
        out += [manifest] + [prefix + n for n in names]
    return out


def collect_so(build_dir: Path) -> list:
    """Every *.so under build_dir except inside CMakeFiles/ directories.

    Searched recursively (not just build/quest/lib) so libraries built by
    add_subdirectory elsewhere in the tree, such as libopenxr_loader.so, are
    packaged too.
    """
    return sorted(p for p in Path(build_dir).rglob("*.so") if "CMakeFiles" not in p.parts)


def _keystore(build_dir: Path) -> Path:
    ks = build_dir / "debug.keystore"
    if not ks.exists():
        keytool = Path(os.environ["JAVA_HOME"]) / "bin" / "keytool"
        subprocess.run([str(keytool), "-genkeypair", "-keystore", str(ks), "-alias", "androiddebugkey",
                        "-storepass", "android", "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048",
                        "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"], check=True)
    return ks


def package(build_dir: Path, so_paths: list, assets_dir: "Path | None", out_apk: Path) -> None:
    build_dir = Path(build_dir)
    tools = Path(os.environ["ANDROID_BUILD_TOOLS"])
    base = build_dir / "base.apk"
    aligned = build_dir / "aligned.apk"
    out_apk = Path(out_apk)
    for f in (base, aligned, out_apk):
        f.unlink(missing_ok=True)
    subprocess.run(aapt2_link_cmd(tools / "aapt2", MANIFEST, os.environ["ANDROID_JAR"], assets_dir, base),
                   check=True)
    with zipfile.ZipFile(base, "a", zipfile.ZIP_DEFLATED) as z:
        for so in so_paths:
            z.write(so, f"lib/arm64-v8a/{Path(so).name}")
        if assets_dir is not None:
            # aapt2 link -A drops dotfiles (the packs' .sm64ar-verified stamps), dot- and _* directories and *~ files:
            # whatever the manifests list that it left out is added here, then verified.
            for rel in missing_assets(listed_assets(assets_dir), z.namelist()):
                if (Path(assets_dir) / rel).is_file():
                    z.write(Path(assets_dir) / rel, "assets/" + rel)
            still = missing_assets(listed_assets(assets_dir), z.namelist())
            if still:
                raise SystemExit(f"quest_package: {len(still)} listed assets are not in the APK: {still[:5]}")
            print(f"assets: all {len(listed_assets(assets_dir))} listed files are in the APK (0 missing)")
    subprocess.run([str(tools / "zipalign"), "-f", "-p", "4", str(base), str(aligned)], check=True)
    ks = _keystore(build_dir)
    subprocess.run([str(tools / "apksigner"), "sign", "--ks", str(ks), "--ks-pass", "pass:android",
                    "--key-pass", "pass:android", "--ks-key-alias", "androiddebugkey",
                    "--out", str(out_apk), str(aligned)], check=True)
