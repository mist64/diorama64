#!/usr/bin/env python3
"""scripts/packs.py — optional HD texture and 3D model packs. Packs are untrusted fan-made data derived from
Nintendo's assets: they live only in local/packs/ (git-ignored), are never committed, redistributed, executed or imported.
Always run as `python3 -I scripts/packs.py ...` (nothing from a pack's folder can be imported: the one helper module,
scripts/sm64ar_common.py, is loaded by its explicit path).

  python3 -I scripts/packs.py pin-from-sources            # local/packs/SOURCES.md -> local/packs/manifest.json (local only)
  python3 -I scripts/packs.py list                        # the pinned packs and what is prepared
  python3 -I scripts/packs.py fetch ID                    # download (official pin, or your SOURCES.md row) if missing, check the SHA-256, extract safely
  python3 -I scripts/packs.py prepare ID                  # texture pack: filter, validate, resize (sips/magick), .tex via the harness/pack tool, verify
  python3 -I scripts/packs.py prepare ID [--with-bowser] [--no-mario]   # model pack: curated bins walked, PNGs re-encoded, verify
  python3 -I scripts/packs.py prepare render96-models-balanced   # variant: per-actor mix of two prepared packs by cost, verify
                                                          # (needs local/packs/actor_costs.json: python3 scripts/pack_costs.py)
  python3 -I scripts/packs.py verify ID                   # re-check every prepared file, write prepared/<name>.verified.json
  python3 -I scripts/packs.py report                      # the decoded MB of every prepared pack (from its .verified.json)
  python3 -I scripts/packs.py stage-harness SAVE          # SAVE/dynos/packs/<name> -> the verified prepared folders
"""
import hashlib, importlib.util, json, os, pathlib, re, shutil, stat, struct, subprocess, sys, tempfile, urllib.request, \
    zipfile, zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def _load_common():
    """scripts/sm64ar_common.py by its explicit path (-I leaves scripts/ off sys.path, and nothing else is searched)."""
    if "sm64ar_common" in sys.modules:
        return sys.modules["sm64ar_common"]
    spec = importlib.util.spec_from_file_location("sm64ar_common", ROOT / "scripts" / "sm64ar_common.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["sm64ar_common"] = mod
    spec.loader.exec_module(mod)
    return mod


common = _load_common()
PACKS = ROOT / "local" / "packs"
MANIFEST = PACKS / "manifest.json"
SOURCES = PACKS / "SOURCES.md"
KNOWN = {
    "sm64-reloaded": ("textures", "SM64 Reloaded"),
    "render96-hd-textures": ("textures", "Render96 HD"),
    "render96-models": ("models", "Render96 Models"),
    "swagskeleton95-replacements": ("models", "SwagSkeleton95"),
}
# Pinned to the release we use: (https URL, bytes, sha256). A row of the local SOURCES.md overrides it.
OFFICIAL = {
    "sm64-reloaded": ("https://github.com/GhostlyDark/SM64-Reloaded/releases/download/v2.6.0/sm64-reloaded-v2.6.0-sm64ex-hd.zip",
                      211176212, "c684e3565ee4b1e252faccdb70d33d1ed26eecce814a9d6916ee682790ad49d8"),
    "render96-hd-textures": ("https://github.com/pokeheadroom/RENDER96-HD-TEXTURE-PACK/releases/download/1.3.26.7.7/"
                             "Render96.HD.Texture.Pack.1.3.26.7.7.7z",
                             391824927, "a9ea999c8a4a2bd68954b0f9eecee62db481ef31f6189cf362cc72c9edc4191c"),
    "render96-models": ("https://github.com/Render96/ModelPack/releases/download/3.25/Render96_DynOs_v3.25.7z",
                        93050150, "22eea6dafcc0a87659d60513eb6a6b0f4ea0acbc6645a6b1b445de9e5812e6df"),
    "swagskeleton95-replacements": ("https://mods.sm64coopdx.com/mods/dynos-swagskeleton95s-replacements.564/download",
                                    466405, "277bd30069ffca3338544dca30cdade4cf736327df4b8663b2bc5fa31cf6f345"),
}


def url_file_name(pid, url):
    return pid + ".zip" if url.endswith("/download") else url.rstrip("/").split("/")[-1]


def official_entry(pid):
    url, size, sha = OFFICIAL[pid]
    kind, name = KNOWN[pid]
    return {"url": url, "bytes": size, "sha256": sha, "kind": kind, "name": name, "file": url_file_name(pid, url)}


VARIANTS = {
    "render96-models-balanced": {"name": "Render96 Models (Balanced)", "base": "render96-models",
                                 "fallback": "swagskeleton95-replacements"},
}
ALLOWED_EXT = {".png", ".bin", ".tex", ".txt", ".md"}
MAX_MEMBERS = 10000
MAX_UNPACKED = 4 << 30
MAX_ARCHIVE = 1 << 30
BSDTAR = shutil.which("bsdtar") or "/usr/bin/bsdtar"


class Refused(Exception):
    """The input is refused; nothing past quarantine was written."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def load_manifest(path=None):
    path = pathlib.Path(path or MANIFEST)
    m = {pid: official_entry(pid) for pid in OFFICIAL}
    m.update(json.loads(path.read_text()) if path.exists() else {})
    for vid, v in VARIANTS.items():
        if v["base"] in m and v["fallback"] in m:
            m[vid] = {"kind": "models", "name": v["name"], "variant": True, "base": v["base"], "fallback": v["fallback"]}
    return m


def pin_from_sources(sources_md=SOURCES, manifest_path=MANIFEST):
    m = {}
    for line in pathlib.Path(sources_md).read_text().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4 or cells[0] not in KNOWN or not re.fullmatch(r"[0-9a-f]{64}", cells[3]) or not cells[2].isdigit():
            continue
        kind, name = KNOWN[cells[0]]
        if cells[1].startswith("/"):
            file = local_file_name(cells[0], cells[1])
        elif cells[1].endswith("/download"):
            file = cells[0] + ".zip"
        elif _file_ok(cells[1].rstrip("/").split("/")[-1]):
            file = cells[1].rstrip("/").split("/")[-1]
        else:
            continue
        m[cells[0]] = {"url": cells[1], "bytes": int(cells[2]), "sha256": cells[3], "kind": kind, "name": name, "file": file}
    pathlib.Path(manifest_path).write_text(json.dumps(m, indent=2, sort_keys=True) + "\n")
    return m


def _file_ok(name):
    return bool(re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9._-]*", name)) and name not in (".", "..")


def local_file_name(pid, path):
    suffix = pathlib.PurePath(path).suffix.lower()
    return pid + (suffix if suffix in (".zip", ".7z", ".rar") else ".zip")


class _HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to another https URL."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise Refused(f"{req.full_url}: redirected to {newurl!r}; only https downloads")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def https_open(url, timeout=60):
    """urlopen for downloads: https, and every redirect https too."""
    if not url.startswith("https://"):
        raise Refused(f"{url}: only https downloads")
    # a plain User-Agent: some hosts refuse urllib's default one (HTTP 403)
    req = urllib.request.Request(url, headers={"User-Agent": "Diorama64-build"})
    try:
        return urllib.request.build_opener(_HttpsOnlyRedirects).open(req, timeout=timeout)
    except OSError as e:
        raise Refused(f"{url}: download failed ({getattr(e, 'reason', None) or e})") from None


def fetch(entry, pack_dir, opener=https_open):
    """Downloaded over https (or copied from a local path) if missing; refused unless its SHA-256 is the pinned one."""
    pack_dir = pathlib.Path(pack_dir)
    pack_dir.mkdir(parents=True, exist_ok=True)
    if not _file_ok(entry["file"]):
        raise Refused(f"{entry['file']!r}: not a plain file name")
    dst = pack_dir / entry["file"]
    if dst.exists():
        if sha256_file(dst) == entry["sha256"]:
            return dst
        raise Refused(f"{dst}: SHA-256 differs from the pinned one (delete it to download again)")
    local = entry["url"].startswith("/")
    if not local and not entry["url"].startswith("https://"):
        raise Refused(f"{entry['url']}: only https downloads or a local archive path")
    part = pack_dir / (entry["file"] + ".part")
    h, n = hashlib.sha256(), 0
    try:
        with (open(entry["url"], "rb") if local else opener(entry["url"], timeout=60)) as r, open(part, "wb") as f:
            for b in iter(lambda: r.read(1 << 20), b""):
                n += len(b)
                if n > MAX_ARCHIVE:
                    raise Refused(f"{entry['url']}: larger than {MAX_ARCHIVE} bytes")
                h.update(b)
                f.write(b)
        if h.hexdigest() != entry["sha256"]:
            raise Refused(f"{entry['url']}: SHA-256 {h.hexdigest()} is not the pinned {entry['sha256']}")
        part.rename(dst)
        return dst
    except OSError as e:
        raise Refused(f"{entry['url']}: {e}") from None
    finally:
        part.unlink(missing_ok=True)


def _member_ok(name):
    if not name or "\x00" in name or name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name):
        return False
    parts = re.split(r"[\\/]", name)
    return ".." not in parts


def _list_7z(archive):
    names = subprocess.run([BSDTAR, "-tf", str(archive)], capture_output=True, text=True, check=True).stdout.splitlines()
    verbose = subprocess.run([BSDTAR, "-tvf", str(archive)], capture_output=True, text=True, check=True).stdout.splitlines()
    if len(names) != len(verbose):
        raise Refused(f"{archive}: listing mismatch (names with newlines?)")
    out = []
    for name, v in zip(names, verbose):
        f = v.split()
        if not f or f[0][0] not in "-d":
            raise Refused(f"{archive}: {name}: not a regular file or directory ({f[0] if f else '?'})")
        if len(f) < 5 or not f[4].isdigit():
            raise Refused(f"{archive}: {name}: unparseable size field")
        out.append((name, f[0][0] == "d", int(f[4])))
    return out


def _is_junk(name):
    parts = re.split(r"[\\/]", name)
    return parts[0] == "__MACOSX" or parts[-1].startswith("._")


def safe_extract(archive, dest, log):
    """Untrusted-input errors (corrupt archive, extractor failure, path clashes) are all reported as Refused."""
    try:
        return _safe_extract(archive, dest, log)
    except (zipfile.BadZipFile, subprocess.CalledProcessError, OSError) as e:
        raise Refused(f"{archive}: {type(e).__name__}: {e}") from e


def _safe_extract(archive, dest, log):
    """Lists first (refuses traversal, links, devices, > MAX_MEMBERS, > MAX_UNPACKED), extracts into a fresh quarantine,
    re-checks every path with lstat, then moves only ALLOWED_EXT files into dest. Returns counts and skipped members."""
    archive, dest = pathlib.Path(archive), pathlib.Path(dest)
    with open(archive, "rb") as fh:
        head = fh.read(6)
    if head[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(archive) as z:
            infos = z.infolist()
            members = []
            for i in infos:
                mode = (i.external_attr >> 16) & 0o170000
                if mode and mode not in (stat.S_IFREG, stat.S_IFDIR):
                    raise Refused(f"{archive}: {i.filename}: link or special file")
                members.append((i.filename, i.is_dir(), i.file_size))
    elif head == b"7z\xbc\xaf\x27\x1c":
        members = _list_7z(archive)
    else:
        raise Refused(f"{archive}: not a zip or 7z archive")
    if len(members) > MAX_MEMBERS:
        raise Refused(f"{archive}: {len(members)} members (limit {MAX_MEMBERS})")
    if sum(m[2] for m in members) > MAX_UNPACKED:
        raise Refused(f"{archive}: more than {MAX_UNPACKED} bytes unpacked")
    for name, _, _ in members:
        if not _member_ok(name):
            raise Refused(f"{archive}: unsafe member path {name!r}")
    quarantine = pathlib.Path(tempfile.mkdtemp(prefix="quarantine-", dir=archive.parent))
    try:
        if head[:4] == b"PK\x03\x04":
            written = 0
            with zipfile.ZipFile(archive) as z:
                for i in z.infolist():
                    if i.is_dir() or _is_junk(i.filename):
                        continue
                    out = quarantine / i.filename
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(i) as src, open(out, "wb") as f:
                        # the declared size is untrusted: stream in chunks under the running cap (decompression bomb)
                        for chunk in iter(lambda: src.read(1 << 20), b""):
                            written += len(chunk)
                            if written > MAX_UNPACKED:
                                raise Refused(f"{archive}: more than {MAX_UNPACKED} bytes unpacked")
                            f.write(chunk)
        else:
            subprocess.run([BSDTAR, "-x", "-f", str(archive), "-C", str(quarantine), "--no-same-owner", "--no-same-permissions",
                            "--no-fflags", "--no-xattrs", "--no-acls"],
                           check=True)
        files, total, skipped = [], 0, []
        for dirpath, dirnames, filenames in os.walk(quarantine, followlinks=False):
            for n in dirnames + filenames:
                st = os.lstat(os.path.join(dirpath, n))
                if stat.S_ISLNK(st.st_mode) or not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
                    raise Refused(f"{archive}: {n}: link or special file after extraction")
                if stat.S_ISREG(st.st_mode):
                    if st.st_nlink > 1:
                        raise Refused(f"{archive}: {n}: hard link")
                    total += st.st_size
                    rel = pathlib.Path(dirpath, n).relative_to(quarantine)
                    if _is_junk(str(rel)):
                        continue
                    if rel.suffix.lower() in ALLOWED_EXT:
                        files.append(rel)
                    else:
                        skipped.append(str(rel))
                        log(f"skipped {rel} (not an allowed type)")
        if total > MAX_UNPACKED:
            raise Refused(f"{archive}: more than {MAX_UNPACKED} bytes unpacked")
        if dest.exists():
            shutil.rmtree(dest)
        for rel in files:
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            os.replace(quarantine / rel, dest / rel)
            os.chmod(dest / rel, 0o644)
        return {"files": len(files), "bytes": total, "skipped": skipped}
    finally:
        shutil.rmtree(quarantine, ignore_errors=True)


DROP_PREFIXES = ("textures/skybox_tiles/", "levels/menu/", "levels/intro/", "textures/intro_raw/", "textures/title_screen_bg/")
# The menu font's glyphs are kept (Story's act selector and file select use it) so they are HD like the dialog and HUD
# fonts.
KEEP_SYMBOL_PREFIXES = ("texture_menu_font_char_",)


def dropped(rel, symbol):
    return rel.startswith(DROP_PREFIXES) and not symbol.startswith(KEEP_SYMBOL_PREFIXES)
SRC = ROOT / "build" / "_src" / "sm64"
ROM = ROOT / "local" / "baserom.us.z64"
TEX_MAX = 1024


US_DEFINES = {"VERSION_US", "ENHANCE_LEVEL_TEXTURES"}


def _cond(expr):
    def one(t):
        t = t.strip()
        neg = t.startswith("!")
        m = re.fullmatch(r"!?\s*(?:defined\s*\(\s*(\w+)\s*\)|(\w+))", t)
        if not m:
            raise Refused(f"unsupported preprocessor condition {expr!r}")
        return ((m.group(1) or m.group(2)) in US_DEFINES) != neg
    return any(all(one(t) for t in alt.split("&&")) for alt in expr.split("||"))


def builtin_textures(src=SRC):
    """A define_builtin_tex_ row (the dialog font's A-Z) is DynOS's name with a trailing "_" (no clash with a-z on a case-insensitive file system)."""
    text = (pathlib.Path(src) / "data" / "dynos_mgr_builtin_tex.cpp").read_text()
    row = re.compile(r'define_builtin_tex(_?)\((\w+), "([^"]+)", (\d+), (\d+),')
    stack, out = [], {}
    for line in text.splitlines():
        d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)", line)
        if d:
            kw, arg = d.group(1), re.sub(r"//.*|/\*.*?\*/", "", d.group(2)).strip()
            parent = stack[-1][0] and stack[-1][1] if stack else True
            if kw in ("if", "ifdef", "ifndef"):
                c = _cond(arg if kw == "if" else ("!" if kw == "ifndef" else "") + arg)
                stack.append([parent, c, c])
            elif not stack:
                raise Refused(f"unbalanced #{kw}")
            elif kw == "endif":
                stack.pop()
            else:
                top = stack[-1]
                c = (not top[2]) and (kw == "else" or _cond(arg))
                top[1], top[2] = c, top[2] or c
                top[0] = stack[-2][0] and stack[-2][1] if len(stack) > 1 else True
            continue
        if stack and not (stack[-1][0] and stack[-1][1]):
            continue
        m = row.search(line)
        if m and not line.lstrip().startswith("//"):
            out[m.group(3)] = (m.group(2) + m.group(1), int(m.group(4)), int(m.group(5)))
    if stack:
        raise Refused("unbalanced #if in the builtin texture table")
    return out


def png_info(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n" or len(data) < 33:
        raise Refused("not a PNG")
    pos, w = 8, None
    while pos + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if pos + 12 + n > len(data):
            raise Refused("truncated PNG chunk")
        body = data[pos + 8:pos + 8 + n]
        if struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0] != (zlib.crc32(kind + body) & 0xffffffff):
            raise Refused(f"bad CRC in {kind!r}")
        if pos == 8:
            if kind != b"IHDR" or n != 13:
                raise Refused("first chunk is not IHDR")
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            if not (1 <= w <= 4096 and 1 <= h <= 4096) or ctype not in (0, 2, 3, 4, 6) or depth not in (1, 2, 4, 8, 16) \
                    or comp or filt or inter not in (0, 1):
                raise Refused(f"unsupported PNG {w}x{h} depth {depth} type {ctype}")
        if kind == b"IEND":
            return w, h, depth, ctype
        pos += 12 + n
    raise Refused("no IEND")


def texture_root(extracted):
    extracted = pathlib.Path(extracted)
    for depth in range(4):
        for d in sorted(extracted.glob("/".join(["*"] * depth)) if depth else [extracted]):
            if d.is_dir() and not d.is_symlink() and any((d / k).is_dir() for k in ("actors", "levels", "textures")):
                return d
    raise Refused(f"{extracted}: no actors/, levels/ or textures/ folder")


def target_size(n64_w, n64_h, png_w, png_h):
    cap = min(4 * max(n64_w, n64_h), TEX_MAX)
    return cap if max(png_w, png_h) > cap else 0


def _resize_png(src, dst, cap):
    """src re-encoded as a PNG at dst (scaled down to cap when cap is given): sips on macOS, ImageMagick on Linux."""
    cmd = common.resize_png_cmd(src, dst, cap)
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        raise Refused(f"{os.path.basename(cmd[0])} failed: {r.stderr.decode('utf-8', 'replace')[:200]}")


def prepare_textures(pid, entry, rom=ROM, src=SRC):
    """Every kept PNG is re-encoded through sips/magick (resized when over target_size), so no raw pack PNG byte reaches the
    generator's stb."""
    table = builtin_textures(src)
    root = texture_root(PACKS / pid / "extracted")
    png_dir = PACKS / pid / "png" / entry["name"]
    out = PACKS / pid / "prepared" / entry["name"]
    for f in (f"{entry['name']}.verified.json", f"{entry['name']}.expected.json"):
        (PACKS / pid / "prepared" / f).unlink(missing_ok=True)
    for d in (png_dir, out):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    rep = {"matched": 0, "dropped": 0, "unmatched": [], "resized": 0, "refused": []}
    for p in sorted(root.rglob("*.png")):
        rel = p.relative_to(root).as_posix()
        if p.is_symlink():
            rep["refused"].append(f"{rel}: symlink")
            continue
        if dropped(rel, table[rel][0] if rel in table else ""):
            rep["dropped"] += 1
            continue
        if rel not in table:
            rep["unmatched"].append(rel)
            continue
        dst, size = png_dir / rel, 0
        try:
            w, h, _, _ = png_info(p.read_bytes())
            dst.parent.mkdir(parents=True, exist_ok=True)
            size = target_size(table[rel][1], table[rel][2], w, h)
            _resize_png(p, dst, size or None)
            png_info(dst.read_bytes())
        except Refused as e:
            rep["refused"].append(f"{rel}: {e}")
            dst.unlink(missing_ok=True)
            continue
        rep["resized"] += bool(size)
        rep["matched"] += 1
    expected = expected_textures(png_dir, table)
    gen_out = tempfile.mkdtemp(prefix="sm64ar-gen-out-")
    try:
        subprocess.run(common.pack_generator_cmd(rom, gen_out) + ["--generate-tex", str(png_dir), str(out)], check=True)
    finally:
        shutil.rmtree(gen_out, ignore_errors=True)
    missing = sorted(expected - {f.stem for f in out.glob("*.tex")})
    if missing:
        raise Refused(f"generation incomplete: {len(missing)} of {len(expected)} textures missing, e.g. {', '.join(missing[:5])}")
    (PACKS / pid / "prepared" / f"{entry['name']}.expected.json").write_text(
        json.dumps(sorted(f"{x}.tex" for x in expected), indent=1) + "\n")
    return rep


def expected_textures(png_dir, table):
    out = set()
    for f in pathlib.Path(png_dir).rglob("*.png"):
        rel = f.relative_to(png_dir).as_posix()
        if rel not in table:
            raise Refused(f"{rel}: a PNG that is not a built-in texture reached png/ (re-run prepare)")
        out.add(table[rel][0])
    return out


def check_tex(data, stem, symbols):
    if len(data) < 2 or data[0] != 21:
        raise Refused("not a raw texture (type 21)")
    n = data[1]
    name = data[2:2 + n].decode("ascii", "replace")
    if name != stem or name not in symbols:
        raise Refused(f"name {name!r} is not the file stem or not a built-in texture")
    if len(data) < 2 + n + 20:
        raise Refused("truncated header")
    fmt, siz, w, h, count = struct.unpack_from("<iiiii", data, 2 + n)
    if fmt != 0 or siz != 3 or not (1 <= w <= TEX_MAX and 1 <= h <= TEX_MAX) or count != w * h * 4 \
            or len(data) != 2 + n + 20 + count:
        raise Refused(f"bad raw texture {w}x{h} fmt {fmt} siz {siz} count {count} size {len(data)}")
    return w, h


def verify(pid, entry, symbols, actors=None, expected=None):
    out = PACKS / pid / "prepared" / entry["name"]
    if not out.is_dir() or not any(out.iterdir()):
        raise Refused(f"{out}: nothing prepared")
    if expected is not None and (gone := sorted(set(expected) - {f.stem for f in out.iterdir()})):
        raise Refused(f"{out}: {len(gone)} expected files missing, e.g. {', '.join(gone[:5])}")
    if pid in VARIANTS:
        check_variant_rule(pid, entry)
    hashes, mb, names = {}, 0.0, None
    excluded = c4_excluded(pid)
    for f in sorted(out.iterdir()):
        if f.is_symlink() or not f.is_file():
            raise Refused(f"{f}: not a regular file")
        if f.stem in excluded:
            raise Refused(f"{f}: excluded for this pack: {excluded[f.stem]}")
        if expected is not None and f.stem not in expected:
            raise Refused(f"{f}: not staged by prepare (not in {entry['name']}.expected.json)")
        data = f.read_bytes()
        if f.suffix == ".tex":
            w, h = check_tex(data, f.stem, symbols)
            mb += w * h * 4 / 1048576
        elif f.suffix == ".bin" and actors is not None:
            names = names or builtin_names()
            mb += check_bin(data, f.stem, actors, names)
        else:
            raise Refused(f"{f}: not a .tex or .bin")
        hashes[f.name] = hashlib.sha256(data).hexdigest()
    (PACKS / pid / "prepared" / f"{entry['name']}.verified.json").write_text(
        json.dumps({"name": entry["name"], "files": hashes, "decoded_mb": round(mb, 1)}, indent=1, sort_keys=True) + "\n")
    return {"files": len(hashes), "decoded_mb": round(mb, 1)}


T_LIGHT, T_TEXTURE, T_VERTEX, T_DL, T_GEO, T_ANIM, T_ANIM_TABLE, T_GFXDYNCMD = 1, 2, 3, 4, 5, 8, 9, 10
T_LIGHT_T, T_AMBIENT_T, T_TEXLIST, T_LIGHT_0 = 18, 19, 20, 24
RECORD = {T_LIGHT: 24, T_LIGHT_0: 24, T_LIGHT_T: 12, T_AMBIENT_T: 8}    # sizeof(Lights1, Lights0, Light_t, Ambient_t)
LIGHT_SELECTORS = {T_LIGHT: {1, 2}, T_LIGHT_0: {1, 2}, T_LIGHT_T: {1, 2, 3}, T_AMBIENT_T: {1, 2}}
NODE_NAMES = {T_LIGHT: "light", T_LIGHT_0: "light0", T_LIGHT_T: "light_t", T_AMBIENT_T: "ambient_t", T_TEXTURE: "texture",
              T_TEXLIST: "texture_list", T_VERTEX: "vertex", T_DL: "display_list", T_GEO: "geo_layout", T_ANIM: "animation",
              T_ANIM_TABLE: "animation_table", T_GFXDYNCMD: "gfxdyncmd"}
# GetPointerFromData's search order over the file's own nodes (dynos_bin_pointer.cpp): the first list holding the name wins.
RESOLVE_ORDER = (T_LIGHT, T_LIGHT_0, T_LIGHT_T, T_AMBIENT_T, T_TEXTURE, T_TEXLIST, T_DL, T_GEO, T_VERTEX)
PNTR, FUNC, LUAV, TREF = 0x52544E50, 0x434E5546, 0x5641554C, 0x52584554
F32_SENTINEL = (0x3346, 0x5632, 0x5854)
BIN_MAX = 40 << 20          # bytes: a bin, compressed header's claim or inflated
ARRAY_MAX = 1 << 20         # elements in any one array node
NAME_MAX = 254              # bytes in a node or pointer name
BIN_PNG_MAX = 512           # textures embedded in one model bin
# Display lists are an ALLOWLIST of F3DEX2 commands (GRUCODE f3dex2e, GBI_NO_MULTI_COMMANDS; gfx_run_dl in
# src/pc/gfx/gfx_pc.c): exactly the opcodes the curated Render96 and Swag bins use, each with every w0/w1 field the
# renderer reads checked in _dl_command. Anything else (the coopdx extensions G_VTX_EXT, G_TEXOVERRIDE_DJUI, ...,
# G_NOOP's AR tags, G_MTX, G_SETCIMG/ZIMG, rectangles) is refused.
G_VTX, G_TRI1, G_TRI2, G_COPYMEM, G_TEXTURE, G_GEOMETRYMODE = 0x01, 0x05, 0x06, 0xD2, 0xD7, 0xD9
G_MOVEWORD, G_MOVEMEM, G_DL, G_ENDDL, G_SETOTHERMODE_L, G_SETOTHERMODE_H = 0xDB, 0xDC, 0xDE, 0xDF, 0xE2, 0xE3
G_RDPLOADSYNC, G_RDPPIPESYNC, G_RDPTILESYNC, G_LOADTLUT, G_SETTILESIZE, G_LOADBLOCK = 0xE6, 0xE7, 0xE8, 0xF0, 0xF2, 0xF3
G_LOADTILE, G_SETTILE, G_SETPRIMCOLOR, G_SETENVCOLOR, G_SETCOMBINE, G_SETTIMG = 0xF4, 0xF5, 0xFA, 0xFB, 0xFC, 0xFD
G_MTX = 0xDA
# G_SETTIMG names a texture node of the file (DynOS_Tex_Import uploads it) or NULL (skipped), never a built-in texture's raw
# pixels: a load's size comes from the command, so a built-in one could be read past its end. No curated bin needs one.
DL_POINTER_KINDS = {G_VTX: {T_VERTEX}, G_DL: {T_DL}, G_SETTIMG: {T_TEXTURE},
                    G_MOVEMEM: {T_LIGHT, T_LIGHT_0, T_LIGHT_T, T_AMBIENT_T}}
DL_RAW = {G_TRI1, G_TRI2, G_COPYMEM, G_TEXTURE, G_GEOMETRYMODE, G_MOVEWORD, G_ENDDL, G_SETOTHERMODE_L, G_SETOTHERMODE_H,
          G_RDPLOADSYNC, G_RDPPIPESYNC, G_RDPTILESYNC, G_LOADTLUT, G_SETTILESIZE, G_LOADBLOCK, G_LOADTILE, G_SETTILE,
          G_SETPRIMCOLOR, G_SETENVCOLOR, G_SETCOMBINE}
DL_ALLOWED = set(DL_POINTER_KINDS) | DL_RAW
DL_LOADS = {G_LOADBLOCK, G_LOADTILE, G_LOADTLUT}   # read rdp.texture_to_load: must come after this list's own G_SETTIMG
DL_NULL_OK = {G_SETTIMG}     # gfx_pc logs "NULL texture" and skips it (Fast64 exports emit gsDPSetTextureImage(..., 0))
MAX_VERTICES, MAX_LIGHTS = 64, 18      # src/pc/gfx/gfx.h: rsp.loaded_vertices[MAX_VERTICES + 4], current_lights[MAX_LIGHTS + 1]
G_MV_LIGHT = 10                        # F3DEX2 movemem/copymem index; light n sits at byte offset 24 * (n + 2)
G_MW_NUMLIGHT, G_MW_FOG, G_MW_LIGHTCOL = 0x02, 0x08, 0x0A
MOVEWORD_OK = {G_MW_NUMLIGHT, G_MW_FOG, G_MW_LIGHTCOL}
LIGHT_T_BYTES = 12
# Bytes left in the node's allocation after the address GetPointerFromData returns for (node type, selector):
# Lights1/Lights0 {Ambient a (8); Light l[1] (16)}, Light_t {col, colc, dir: 4 each}, Ambient_t {col, colc: 4 each}.
LIGHT_BYTES_LEFT = {(T_LIGHT, 1): 16, (T_LIGHT, 2): 24, (T_LIGHT_0, 1): 16, (T_LIGHT_0, 2): 24, (T_LIGHT_T, 1): 12,
                    (T_LIGHT_T, 2): 8, (T_LIGHT_T, 3): 4, (T_AMBIENT_T, 1): 8, (T_AMBIENT_T, 2): 4}
DL_COST_MAX = 1 << 18       # commands one display list executes, pushed and branched-to lists included
DL_DEPTH_MAX = 16           # nested G_DL pushes: gfx_run_dl recurses once per push
PNG_CRITICAL = {b"IHDR", b"PLTE", b"IDAT", b"IEND"}
CURATED = ("mario_geo", "marios_cap_geo", "marios_metal_cap_geo", "marios_wing_cap_geo", "marios_winged_metal_cap_geo",
           "yellow_coin_geo", "yellow_coin_no_shadow_geo", "red_coin_geo", "red_coin_no_shadow_geo", "blue_coin_geo",
           "blue_coin_no_shadow_geo", "bubbly_tree_geo", "spiky_tree_geo", "snow_tree_geo", "palm_tree_geo", "star_geo",
           "exclamation_box_geo", "wooden_signpost_geo", "wooden_door_geo", "castle_door_geo", "goomba_geo",
           "black_bobomb_geo", "bobomb_buddy_geo", "king_bobomb_geo", "chain_chomp_geo", "metallic_ball_geo", "whomp_geo",
           "thwomp_geo", "koopa_with_shell_geo", "koopa_without_shell_geo", "koopa_flag_geo", "piranha_plant_geo",
           "heave_ho_geo", "bowling_ball_geo", "bubba_geo", "penguin_geo", "mr_blizzard_geo", "spindrift_geo", "boo_geo",
           "mad_piano_geo", "bully_geo", "chuckya_geo")
BOWSER = ("bowser_geo", "bowser2_geo")
# Models must never change the game state: pack id -> {actor: reason}. Empty: a pack graph only draws
# (rendering_graph_node.c, 0007-DynOS-packs.patch: the vanilla graph still runs every frame for the game state, the pack
# graph draws in a sandbox).
C4_EXCLUDED = {}
MARIO = tuple(n for n in CURATED if n.startswith(("mario_", "marios_")))
# Geo functions. The game no longer reads anything a pack graph does: in AR an object keeps its vanilla
# graph, which runs every frame for the game state exactly as vanilla, and the pack graph draws afterwards in a sandbox
# that saves and restores everything the allowed functions and the graph processors can write (rendering_graph_node.c,
# 0007-DynOS-packs.patch). So a bin may call any FUNCTION_GEO built-in that is memory-safe on the graph node its command creates and
# whose writes the sandbox covers. GEO_FUNCS: function -> (geo command, the command that must create the node after it
# (node->next, a sibling ring) when the function casts node->next, else None). The sandbox covers: the drawn object and
# its prevObj (geo_update_*_from_parent, geo_move_mario_part_from_parent), a held object and the holder's held node,
# MarioState/MarioBodyState of the players the functions index, mario_misc.c's attack counter and player colours, the
# room globals and find_floor's flag and counters (geo_switch_area, shadows), the shadow and animation globals, and the
# vanilla graph of a held object drawn inside a pack draw. Functions that cast gCurGraphNodeObject to struct Object are
# safe: pack graphs draw for object-pool objects only (mirror Mario keeps the vanilla graph). geo_switch_mario_hand_grab_pos
# in any actor's GEO_HELD_OBJECT draws Mario's held object there (an actor holding itself would recurse: the renderer
# stops held draws nested 4 deep inside a pack draw).
#
# GEO_DENIED: every other FUNCTION_GEO built-in, with why (memory-unsafe, or effects the sandbox does not capture).
_MOVTEX = "moving_texture.c globals and the static moving-texture vertex data"
GEO_DENIED = {
    "geo_render_mirror_mario": "CREATE re-initialises the global gMirrorMario[] nodes at load (outside any frame's "
                               "sandbox); AREA_LOAD links them into the level graph",
    "geo_camera_main": "needs a GraphNodeCamera; runs the camera update",
    "geo_camera_fov": "needs a GraphNodePerspective (writes fov past any other node)",
    "geo_skybox_main": "needs a background node",
    "geo_envfx_main": "advances the RNG (random_u16 also reseeds sync objects) and keeps heap particle buffers",
    "geo_wdw_set_initial_water_level": "CREATE clears gWdwWaterLevelSet at load; RENDER rewrites gEnvironmentRegions",
    "geo_movtex_pause_control": _MOVTEX, "geo_movtex_draw_water_regions": _MOVTEX, "geo_movtex_draw_nocolor": _MOVTEX,
    "geo_movtex_draw_colored": _MOVTEX, "geo_movtex_draw_colored_no_update": _MOVTEX,
    "geo_movtex_draw_colored_2_no_update": _MOVTEX, "geo_movtex_update_horizontal": _MOVTEX,
    "geo_movtex_draw_water_regions_ext": _MOVTEX,
    "geo_painting_draw": "indexes sPaintingGroups[(param >> 8) & 0xFF] unchecked; painting state",
    "geo_painting_update": "the painting state machine (feeds the WDW water level)",
    "geo_exec_flying_carpet_timer_update": "geo_misc.c carpet statics",
    "geo_exec_flying_carpet_create": "geo_misc.c carpet statics (gFlyingCarpetState)",
    "geo_intro_super_mario_64_logo": "intro.c statics (title frame counters)",
    "geo_intro_tm_copyright": "intro.c statics (title frame counters)",
    "geo_intro_regular_backdrop": "intro.c statics; needs the intro's background node",
    "geo_draw_mario_head_goddard": "runs the Goddard renderer (its own state machine and heap)",
}
GEO_ASM_FUNCS = ("geo_update_layer_transparency", "geo_update_held_mario_pos", "geo_update_projectile_pos_from_parent",
                 "geo_update_body_rot_from_parent", "geo_move_mario_part_from_parent", "geo_bits_bowser_coloring",
                 "geo_mirror_mario_set_alpha", "geo_mirror_mario_backface_culling", "geo_mario_set_player_colors",
                 "geo_exec_inside_castle_light", "geo_exec_cake_end_screen", "geo_cannon_circle_base",
                 "geo_mario_cap_display_list")
GEO_SWITCH_FUNCS = ("geo_switch_anim_state", "geo_switch_area", "geo_switch_tuxie_mother_eyes", "geo_switch_bowser_eyes",
                    "geo_switch_mario_stand_run", "geo_switch_mario_eyes", "geo_switch_mario_hand",
                    "geo_switch_mario_cap_effect", "geo_switch_mario_cap_on_off", "geo_switch_peach_eyes",
                    "geo_choose_area_ext", "geo_switch_character_type")
# Geo layout commands (src/engine/geo_layout.c GeoLayoutJumpTable; little-endian CMD_BBH: opcode bits 0-7, param byte
# 8-15, s16 16-31). GeoLayoutJumpTable has 37 entries and process_geo_layout indexes it unchecked.
GEO_BRANCH_AND_LINK, GEO_END, GEO_BRANCH, GEO_RETURN, GEO_OPEN, GEO_CLOSE = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
GEO_NODE_START, GEO_ZBUFFER, GEO_RENDER_RANGE, GEO_SWITCH_CASE = 0x0B, 0x0C, 0x0D, 0x0E
GEO_TRANSLATE_ROTATE, GEO_TRANSLATE_NODE, GEO_ROTATION_NODE, GEO_ANIMATED_PART = 0x10, 0x11, 0x12, 0x13
GEO_BILLBOARD, GEO_DISPLAY_LIST, GEO_SHADOW, GEO_ASM, GEO_HELD_OBJECT, GEO_SCALE = 0x14, 0x15, 0x16, 0x18, 0x1C, 0x1D
GEO_CULLING_RADIUS, GEO_BONE = 0x20, 0x24
GEO_FUNCS = {**{f: (GEO_ASM, None) for f in GEO_ASM_FUNCS}, **{f: (GEO_SWITCH_CASE, None) for f in GEO_SWITCH_FUNCS},
             # these cast node->next: the next sibling must be the node they write
             "geo_rotate_coin": (GEO_ASM, GEO_ROTATION_NODE), "geo_mario_tilt_torso": (GEO_ASM, GEO_ROTATION_NODE),
             "geo_mario_head_rotation": (GEO_ASM, GEO_ROTATION_NODE),
             "geo_mario_rotate_wing_cap_wings": (GEO_ASM, GEO_ROTATION_NODE),
             "geo_mario_hand_foot_scaler": (GEO_ASM, GEO_SCALE), "geo_snufit_scale_body": (GEO_ASM, GEO_SCALE),
             "geo_scale_bowser_key": (GEO_ASM, GEO_SCALE), "geo_snufit_move_mask": (GEO_ASM, GEO_TRANSLATE_ROTATE),
             "geo_offset_klepto_held_object": (GEO_ASM, GEO_TRANSLATE_ROTATE),
             "geo_switch_mario_hand_grab_pos": (GEO_HELD_OBJECT, None)}
# GEO_ASM/GEO_SWITCH_CASE parameter bounds (inclusive, the s16 read as unsigned 0..max): a parameter that indexes memory.
# geo_mario_set_player_colors: layer = param - 3 -> flags >> 8 -> listHeads[8]; the vanilla geos use 0 and layer + 3
# (4, 7, 8). geo_mirror_mario_backface_culling: layer = param >> 2 (mirror Mario only). geo_mario_hand_foot_scaler:
# gMarioAttackScaleAnimation[param * 6 + ..] (18 entries). Every other allowlisted parameter is compared or masked.
GEO_PARAM_MAX = {"geo_mario_set_player_colors": 10, "geo_mirror_mario_backface_culling": 31,
                 "geo_mario_hand_foot_scaler": 2}
# The commands the curated bins use (scanned); the rest (views, cameras, backgrounds, Lua commands, ...) are refused.
# Each one's words are checked in geo_layout, its effect in run_geo.
GEO_ALLOWED = {GEO_BRANCH_AND_LINK, GEO_END, GEO_BRANCH, GEO_RETURN, GEO_OPEN, GEO_CLOSE, GEO_NODE_START,
               GEO_RENDER_RANGE, GEO_SWITCH_CASE, GEO_TRANSLATE_ROTATE, GEO_TRANSLATE_NODE, GEO_ROTATION_NODE, GEO_ANIMATED_PART, GEO_BILLBOARD,
               GEO_DISPLAY_LIST, GEO_SHADOW, GEO_ASM, GEO_HELD_OBJECT, GEO_SCALE, GEO_CULLING_RADIUS}
GEO_DRAWS = {GEO_TRANSLATE_ROTATE, GEO_TRANSLATE_NODE, GEO_ROTATION_NODE, GEO_ANIMATED_PART, GEO_BILLBOARD,
             GEO_DISPLAY_LIST, GEO_SCALE, GEO_BONE}
GEO_LAYER_LOW_NIBBLE = {GEO_TRANSLATE_ROTATE, GEO_TRANSLATE_NODE, GEO_ROTATION_NODE, GEO_BILLBOARD, GEO_SCALE}
GFX_NUM_MASTER_LISTS = 8
# create_shadow_below_xyz's cases (src/game/shadow.h); any other type indexes rectangles[2] with type - 50, unchecked
SHADOW_TYPES = {0, 1, 2, 3, 10, 11, 12, 50, 51, 99}
GEO_STACK, GEO_NODE_DEPTH = 16, 32  # gGeoLayoutStack[16], gCurGraphNodeList[32]
GEO_COST_MAX = 1 << 16      # geo commands one actor's layout executes, branches included
GEO_FUNC_MACROS = ("GEO_ASM", "GEO_SWITCH_CASE", "GEO_HELD_OBJECT", "GEO_CAMERA", "GEO_CAMERA_FRUSTUM_WITH_FUNC",
                   "GEO_BACKGROUND", "GEO_BACKGROUND_EXT")


def _strip_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def builtin_names(src=SRC):
    src = pathlib.Path(src)
    b = _strip_comments((src / "data" / "dynos_mgr_builtin.cpp").read_text())
    head, funcs_part = b.split("sDynosBuiltinFuncs[] = {", 1)
    funcs_part = funcs_part.split("};", 1)[0]
    func_types = {m.group(1): int(m.group(2)) for m in
                  re.finditer(r"#define (FUNCTION_\w+)\s+(\d+)", (src / "data" / "dynos.cpp.h").read_text())}
    rows = re.findall(r"define_builtin_function\(\s*(\w+)\s*,\s*(FUNCTION_\w+)\s*\)|\{\s*\.name\s*=\s*\"(\w+)\"[^}]*?"
                      r"\.type\s*=\s*(FUNCTION_\w+)\s*\}", funcs_part)
    if len(rows) != funcs_part.count("define_builtin_function(") + funcs_part.count(".name"):
        raise Refused("unparsed rows in sDynosBuiltinFuncs")
    funcs = [func_types[t1 or t2] for _, t1, _, t2 in rows]
    func_names = [n1 or n2 for n1, _, n2, _ in rows]
    actors_part = head.split("sDynosBuiltinActors[] = {", 1)[1].split("};", 1)[0]
    tables = set(re.findall(r"define_builtin(?:_ptr)?\((\w+)\)", head))
    behaviors = set(re.findall(r"\bid_(bhv\w+)", (src / "include" / "behavior_table.h").read_text()))
    textures = {v[0] for v in builtin_textures(src).values()}
    return {"actors": set(re.findall(r"define_builtin\((\w+)\)", actors_part)), "pointers": tables | behaviors | textures,
            "textures": textures, "funcs": funcs, "func_types": func_types, "func_names": func_names}


def inflate_bin(data):
    """A DYNOSBIN-compressed bin ("DYNOSBIN", u64 size, zlib) inflated under BIN_MAX, never past its declared size;
    an uncompressed bin unchanged. The device then never inflates an untrusted size."""
    if data[:8] != b"DYNOSBIN":
        return data
    if len(data) < 16:
        raise Refused("truncated compressed bin")
    size = struct.unpack_from("<Q", data, 8)[0]
    if size > BIN_MAX:
        raise Refused(f"compressed bin claims {size} bytes (limit {BIN_MAX})")
    try:
        d = zlib.decompressobj()
        out = d.decompress(data[16:], size + 1)      # a zlib bomb stops one byte past the claim
    except zlib.error as e:
        raise Refused(f"cannot inflate: {e}")
    if len(out) != size or not d.eof or d.unconsumed_tail:
        raise Refused("inflated size differs from the header")
    return out


def png_check(data, cap=None):
    """png_info plus what stb_image will do with it: only the four critical chunks (no CgBI), a palette for type 3,
    no interlace, and IDAT inflating to exactly the bytes the header implies with valid filter bytes. With cap, a side
    over cap px is refused before anything is inflated. -> (w, h)."""
    w, h, depth, ctype = png_info(data)
    if cap and max(w, h) > cap:
        raise Refused(f"PNG {w}x{h} over the {cap} px cap")
    pos, kinds, idat = 8, set(), []
    while True:
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if not kind[0] & 0x20 and kind not in PNG_CRITICAL:
            raise Refused(f"unknown critical PNG chunk {kind!r}")
        kinds.add(kind)
        if kind == b"IDAT":
            idat.append(data[pos + 8:pos + 8 + n])
        if kind == b"IEND":
            break
        pos += 12 + n
    if data[pos + 12:]:
        raise Refused("bytes after IEND")
    if data[28] != 0:
        raise Refused("interlaced PNG")
    if ctype == 3 and b"PLTE" not in kinds:
        raise Refused("palette PNG without PLTE")
    if (ctype == 3 and depth == 16) or (ctype in (2, 4, 6) and depth < 8):
        raise Refused(f"invalid PNG depth {depth} for colour type {ctype}")
    row = (w * {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype] * depth + 7) // 8
    want = h * (row + 1)
    try:
        d = zlib.decompressobj()
        px = d.decompress(b"".join(idat), want + 1)
    except zlib.error as e:
        raise Refused(f"PNG pixel data: {e}")
    if len(px) != want or not d.eof:
        raise Refused("PNG pixel data size differs from the header")
    if any(px[i] > 4 for i in range(0, want, row + 1)):
        raise Refused("bad PNG filter byte")
    return w, h


class _R:
    def __init__(self, data):
        self.d, self.p = data, 0

    def left(self):
        return len(self.d) - self.p

    def take(self, n):
        if n < 0 or n > self.left():
            raise Refused(f"truncated at {self.p} (+{n})")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def s32(self):
        return struct.unpack("<i", self.take(4))[0]

    def name(self):
        """A DynOS String: refused with a NUL (the device compares with strcmp, so "v\\0x" would alias "v") or over
        NAME_MAX bytes (String::Read writes mBuffer[length] = 0)."""
        b = self.take(self.u8())
        if b"\x00" in b or len(b) > NAME_MAX:
            raise Refused(f"name {b[:32]!r}: NUL byte or longer than {NAME_MAX} bytes")
        return b.decode("latin-1")

    def count(self, what, min_bytes):
        k = self.u32()
        if k > ARRAY_MAX or k * min_bytes > self.left():
            raise Refused(f"{what} count {k} larger than the file allows")
        return k


def _dl_command(w0, w1):
    """One display-list command against what gfx_run_dl (F3DEX2) does with it; w1 as pointer() returned it.
    -> the (cost, depth) of the list it pushes or branches to, else None. Refused: an opcode not in DL_ALLOWED, a node
    pointer where the command takes a value and the reverse, and every field that would index past a renderer array."""
    op = w0 >> 24
    c0 = lambda pos, width: (w0 >> pos) & ((1 << width) - 1)
    if op not in DL_ALLOWED:
        raise Refused(f"display-list opcode 0x{op:02x} not allowed (w0 0x{w0:08x})")
    if op in DL_RAW:
        if w1[0] is not None:
            raise Refused(f"opcode 0x{op:02x} takes a value, not a pointer")
        v = w1[1]
        c1 = lambda pos, width: (v >> pos) & ((1 << width) - 1)
        if op == G_TRI1 and max(c0(16, 8), c0(8, 8), c0(0, 8)) // 2 >= MAX_VERTICES:
            raise Refused(f"triangle vertex index past {MAX_VERTICES} (w0 0x{w0:08x})")
        if op == G_TRI2 and max(c0(16, 8), c0(8, 8), c0(0, 8), c1(16, 8), c1(8, 8), c1(0, 8)) // 2 >= MAX_VERTICES:
            raise Refused(f"triangle vertex index past {MAX_VERTICES} (0x{w0:08x} 0x{v:08x})")
        if op == G_COPYMEM and (c0(0, 8) != G_MV_LIGHT or not all(0 <= c0(p, 8) * 8 // 24 - 2 <= MAX_LIGHTS for p in (8, 16))):
            raise Refused(f"copymem outside the light array (w0 0x{w0:08x})")
        if op == G_MOVEWORD:
            if c0(16, 8) not in MOVEWORD_OK:
                raise Refused(f"moveword index {c0(16, 8)} not allowed")
            if c0(16, 8) == G_MW_NUMLIGHT and v // 24 > MAX_LIGHTS:
                raise Refused(f"G_MW_NUMLIGHT {v} sets more than {MAX_LIGHTS} lights")
        if op in (G_SETOTHERMODE_L, G_SETOTHERMODE_H) and c0(8, 8) + c0(0, 8) > 31:
            raise Refused(f"othermode field outside its word (w0 0x{w0:08x})")
        return None
    if w1[0] is None:
        if not (w1[1] == 0 and op in DL_NULL_OK):
            raise Refused(f"raw value 0x{w1[1]:08x} where a node pointer is required (w0 0x{w0:08x})")
        return None
    t, detail, sel = w1
    if t not in DL_POINTER_KINDS[op]:
        raise Refused(f"opcode 0x{op:02x} points at a {NODE_NAMES.get(t, t)}, not what it reads")
    if op == G_VTX:
        n, end = c0(12, 8), c0(1, 7)
        if not (1 <= n <= end <= MAX_VERTICES):
            raise Refused(f"vertex load of {n} ending at {end}: outside the {MAX_VERTICES} loaded vertices")
        if sel + n > detail:
            raise Refused(f"vertex offset {sel} (+{n}) past the node ({detail})")
    elif op == G_MOVEMEM:
        if c0(0, 8) != G_MV_LIGHT or not (0 <= c0(8, 8) * 8 // 24 - 2 <= MAX_LIGHTS):
            raise Refused(f"movemem other than a light slot (w0 0x{w0:08x})")
        if LIGHT_BYTES_LEFT[(t, sel)] < LIGHT_T_BYTES:
            raise Refused(f"light movemem reads {LIGHT_T_BYTES} bytes past a {NODE_NAMES[t]} selector {sel}")
    elif op == G_DL:
        return detail
    return None


def display_list(name, cmds):
    """A display list as gfx_run_dl runs it: each command checked, the stream must end, total cost and push depth bounded, a G_DL names only an earlier list, and a list that loads before its own G_SETTIMG never hangs off a geo layout."""
    cost, depth, ended, timg, needs = 0, 1, False, False, False
    for w0, w1 in cmds:
        sub = _dl_command(w0, w1)
        if ended:
            continue
        cost += 1
        op = w0 >> 24
        if op == G_SETTIMG:
            timg = True
        elif op in DL_LOADS:
            needs = needs or not timg
        elif op == G_DL:
            cost += sub[0]
            needs = needs or (sub[2] and not timg)
            if (w0 >> 16) & 1:
                depth, ended = max(depth, sub[1]), True
            else:
                depth = max(depth, sub[1] + 1)
        ended = ended or op == G_ENDDL
    if not ended:
        raise Refused(f"display list {name!r} has no G_ENDDL (it would run past its end)")
    if cost > DL_COST_MAX or depth > DL_DEPTH_MAX:
        raise Refused(f"display list {name!r} executes {cost} commands at nesting {depth} "
                      f"(limits {DL_COST_MAX}, {DL_DEPTH_MAX})")
    return cost, depth, needs


def _geo_shape(op, b):
    dl = lambda k: (k + 1, {k: "dl"}) if b & 0x80 else (k, {})
    if op in (GEO_END, GEO_RETURN, GEO_OPEN, GEO_CLOSE, 0x06, 0x07, 0x09, GEO_NODE_START, GEO_ZBUFFER, 0x17, 0x1B,
              GEO_CULLING_RADIUS):
        return 1, {}
    if op in (GEO_BRANCH_AND_LINK, GEO_BRANCH):
        return 2, {1: "geo"}
    if op in (GEO_SWITCH_CASE, GEO_ASM, 0x19):
        return 2, {1: "func"}
    if op in (GEO_RENDER_RANGE, GEO_SHADOW, 0x1A, 0x1E, 0x22, 0x23):
        return 2, {}
    if op == 0x08:
        return 3, {}
    if op == 0x0A:
        return (3, {2: "func"}) if b else (2, {})
    if op == 0x0F:
        return 5, {4: "func"}
    if op == GEO_TRANSLATE_ROTATE:      # field layouts 4-7 match no case: the command would never advance
        return dl({0: 4, 1: 2, 2: 2, 3: 1}[(b >> 4) & 7]) if (b >> 4) & 7 <= 3 else None
    if op in (GEO_TRANSLATE_NODE, GEO_ROTATION_NODE, GEO_BILLBOARD):
        return dl(2)
    if op == GEO_ANIMATED_PART:
        return 3, {2: "dl"}
    if op == GEO_DISPLAY_LIST:
        return 2, {1: "dl"}
    if op == GEO_HELD_OBJECT:
        return 3, {2: "func"}
    if op == GEO_SCALE:
        return dl(4 if b & 0x40 else 2)
    if op == 0x1F:
        return 4, {}
    if op == 0x21:
        return 3, {1: "geo", 2: "func"}
    if op == GEO_BONE:
        return 5 + (3 if b & 0x80 else 0), {4 + (3 if b & 0x80 else 0): "dl"}
    return None


def geo_commands(name, words):
    out, i = [], 0
    while i < len(words):
        h = words[i]
        if h[0] is not None:
            raise Refused(f"geo layout {name!r} word {i}: a pointer where a command starts")
        op, b, s = h[1] & 0xFF, (h[1] >> 8) & 0xFF, (h[1] >> 16) & 0xFFFF
        shape = _geo_shape(op, b)
        if shape is None:
            raise Refused(f"geo layout {name!r} word {i}: not a geo command (0x{h[1]:08x})")
        k, roles = shape
        if i + k > len(words):
            raise Refused(f"geo layout {name!r}: command 0x{op:02x} at word {i} runs past the node")
        for j in range(1, k):
            if j not in roles and words[i + j][0] is not None:
                raise Refused(f"geo layout {name!r} word {i + j}: a pointer where command 0x{op:02x} takes a value")
        out.append((i, k, op, b, s, {roles[j]: words[i + j] for j in roles}))
        i += k
    return out


def geo_layout(name, words, names):
    """One geo layout node: every command allowed (GEO_ALLOWED), drawing layers inside the master list, jumps only to
    this file's earlier geo layouts, display lists only this file's own (never one that loads a texture it does not
    set), functions only GEO_FUNCS paired with the command (graph node type) they belong to. -> its commands
    (a function that casts node->next gets slots["next"] = (name, the command that must create that node)."""
    cmds = geo_commands(name, words)
    for i, _, op, b, s, slots in cmds:
        if op not in GEO_ALLOWED:
            raise Refused(f"geo layout {name!r} word {i}: geo command 0x{op:02x} not allowed")
        layer = b & 0x0F if op in GEO_LAYER_LOW_NIBBLE or (op == GEO_BONE and b & 0x80) else b
        if op in GEO_DRAWS and layer >= GFX_NUM_MASTER_LISTS:
            raise Refused(f"geo layout {name!r} word {i}: drawing layer {layer} past the {GFX_NUM_MASTER_LISTS} lists")
        if op == GEO_SHADOW and s & 0xFF not in SHADOW_TYPES:
            raise Refused(f"geo layout {name!r} word {i}: shadow type {s & 0xFF} is not one create_shadow_below_xyz knows")
        for role, w in list(slots.items()):
            if role == "geo" and w[0] != T_GEO:
                raise Refused(f"geo layout {name!r} word {i}: jumps to {_what(w)}, not a geo layout of this file")
            if role == "dl":
                if w == (None, 0) and op == GEO_ANIMATED_PART:
                    continue
                if w[0] != T_DL:
                    raise Refused(f"geo layout {name!r} word {i}: draws {_what(w)}, not a display list of this file")
                if w[1][2]:
                    raise Refused(f"geo layout {name!r} draws a display list that loads a texture it never sets")
            if role == "func":
                fname = names["func_names"][w[1]] if w[0] == "func" else None
                if fname is None or GEO_FUNCS.get(fname, (None,))[0] != op:
                    raise Refused(f"geo layout {name!r} word {i}: function {fname or _what(w)} not allowed in geo "
                                  f"command 0x{op:02x} (denied, or not this graph node type)")
                if fname in GEO_PARAM_MAX and not 0 <= s <= GEO_PARAM_MAX[fname]:
                    raise Refused(f"geo layout {name!r} word {i}: {fname} parameter {s} outside 0..{GEO_PARAM_MAX[fname]} "
                                  f"(it indexes memory)")
                if GEO_FUNCS[fname][1] is not None:
                    slots["next"] = (fname, GEO_FUNCS[fname][1])
    return cmds


def _what(w):
    if w[0] == "func":
        return f"function #{w[1]}"
    if w[0] is None:
        return f"value 0x{w[1]:08x}"
    return f"a {NODE_NAMES.get(w[0], w[0])}"


def run_geo(root, geos):
    """process_geo_layout as geo_layout.c runs it, from the actor's root layout (DynOS uses the last one); refuses any path that would overrun the game's geo stack or node list."""
    stack, ret_index, depth, cost = [("ret", None), ("link", (0, 0))], 2, 0, 0
    level = [False] * GEO_NODE_DEPTH
    last = [None] * GEO_NODE_DEPTH
    rings = {}
    node, i = root, 0
    while True:
        cmd = geos[node].get(i)
        if cmd is None:
            raise Refused(f"geo layout #{node} runs past its end (word {i})")
        k, op, b, _, slots = cmd
        if depth < 0 and op not in (GEO_END, GEO_RETURN, GEO_CLOSE):
            raise Refused(f"geo layout #{node}: command 0x{op:02x} after closing below the root")
        cost += 1
        if cost > GEO_COST_MAX:
            raise Refused(f"geo layout #{root} executes more than {GEO_COST_MAX} commands")
        i += k
        if op == GEO_BRANCH_AND_LINK or (op == GEO_BRANCH and b == 1):
            stack.append(("ret", (node, i)))
            if op == GEO_BRANCH_AND_LINK:
                stack.append(("link", (depth, ret_index)))
                ret_index = len(stack)
            if len(stack) > GEO_STACK:
                raise Refused(f"geo layout #{root}: branch stack past {GEO_STACK}")
        if op in (GEO_BRANCH_AND_LINK, GEO_BRANCH):
            node, i = slots["geo"][1], 0
        elif op == GEO_END:
            del stack[ret_index:]
            (depth, ret_index), where = stack.pop()[1], stack.pop()[1]
            if where is None:
                _check_next_nodes(root, rings)
                return cost
            node, i = where
        elif op == GEO_RETURN:
            if stack[-1][0] != "ret":
                raise Refused(f"geo layout #{node}: GEO_RETURN without a matching GEO_BRANCH(1, ...)")
            node, i = stack.pop()[1]
        elif op == GEO_OPEN:
            depth += 1
            if depth >= GEO_NODE_DEPTH:
                raise Refused(f"geo layout #{node}: node nesting past {GEO_NODE_DEPTH - 1}")
            level[depth] = level[depth - 1]
        elif op == GEO_CLOSE:
            depth -= 1      # vanilla castle_door_geo closes one node too many right before GEO_END: harmless
        else:
            if depth and not level[depth - 1]:
                raise Refused(f"geo layout #{node}: a node opened under a level that has no node")
            level[depth] = True
            last[depth] = cost
            rings.setdefault(last[depth - 1] if depth else ("alone", cost), []).append((op, slots.get("next")))


def _check_next_nodes(root, rings):
    for ring in rings.values():
        for k, (_, need) in enumerate(ring):
            if need is not None and ring[(k + 1) % len(ring)][0] != need[1]:
                raise Refused(f"geo layout #{root}: {need[0]} writes the node after it, which is not geo command "
                              f"0x{need[1]:02x}")


def walk_bin(data, names, keep_anims=False, png_cap=None, resize=None):
    """Mirrors DynOS_Actor_LoadFromBinary node by node; refuses anything DynOS would fatal on, read past, mis-resolve or allocate from an unchecked count. Every embedded PNG is re-encoded: no pack PNG byte reaches stb."""
    if len(data) > BIN_MAX:
        raise Refused(f"bin larger than {BIN_MAX} bytes")
    r, out = _R(data), bytearray()
    defs = {}
    info = {"nodes": {}, "pngs": [], "resized": 0, "dropped_anims": 0, "vertices": 0, "triangles": 0}
    anims, geos = set(), []

    def define(n, t, detail=None):
        defs.setdefault(n, {}).setdefault(t, detail)

    def target(n):
        """(file node type, detail) of the file's own node the name resolves to. Anything else is refused, built-ins
        included: GetPointerFromData would hand a geo layout or display list the game's own geo
        layouts (mario_geo and its state-writing functions), level geos, scripts, behaviours, collision or textures."""
        for t in RESOLVE_ORDER:
            if t in defs.get(n, {}):
                return t, defs[n][t]
        if n in names["pointers"]:
            raise Refused(f"pointer to built-in {n!r}: a pack bin may only point at its own nodes")
        raise Refused(f"pointer {n!r} not defined before use and not built in")

    def pointer(func_type, kinds=None, w0=None):
        v = r.u32()
        if v == LUAV:
            raise Refused("Lua token in a pack")
        if v == FUNC:
            i = r.s32()
            if not (0 <= i < len(names["funcs"])) or names["funcs"][i] != func_type:
                raise Refused(f"function index {i} invalid for type {func_type}")
            if kinds is not None or w0 is not None:
                raise Refused("a function where a node pointer is required")
            return "func", i
        if v != PNTR:
            if kinds is not None:
                raise Refused(f"raw value 0x{v:08x} where a node pointer is required")
            return None, v
        n, sel = r.name(), r.u32()
        t, detail = target(n)
        if t in LIGHT_SELECTORS and sel not in LIGHT_SELECTORS[t]:
            raise Refused(f"light selector {sel} for {n}")
        if t == T_VERTEX and w0 is None and sel + 1 > detail:
            raise Refused(f"vertex offset {sel} past {n} ({detail})")
        if kinds is not None and t not in kinds:
            raise Refused(f"pointer {n!r} is a {NODE_NAMES.get(t, t)}, not what the command needs")
        return t, detail, sel

    geo = names.get("func_types", {}).get("FUNCTION_GEO", 1)
    while r.p < len(data):
        start, t = r.p, r.u8()
        if t not in NODE_NAMES:
            raise Refused(f"unknown node type {t} at {start}")
        png_span, n = None, None
        if t == T_ANIM_TABLE:
            a = r.name()
            if a != "NULL" and a not in anims:
                raise Refused(f"animation {a!r} not found")
        elif t == T_GFXDYNCMD:
            r.name(); r.take(5)
        else:
            n, detail = r.name(), None
            if t in RECORD:
                r.take(RECORD[t])
            elif t == T_TEXTURE:
                peek = struct.unpack_from("<I", data, r.p)[0] if r.left() >= 4 else None
                if peek == TREF:
                    r.u32()
                    ref = r.name()
                    if T_TEXTURE not in defs.get(ref, {}):
                        raise Refused(f"texture ref {ref!r} to nothing")
                    detail = defs[ref][T_TEXTURE]
                    if detail:
                        info["pngs"].append(detail)
                else:
                    count = r.s32()
                    if count > r.left():
                        raise Refused(f"texture {n!r} claims {count} bytes")
                    if count > 0:
                        p0 = r.p
                        png = r.take(count)
                        w, h, _, _ = png_info(png)
                        if resize:
                            png = resize(png, png_cap)
                            info["resized"] += bool(png_cap and max(w, h) > png_cap)
                        w, h = png_check(png, png_cap)
                        info["pngs"].append((w, h))
                        detail = (w, h)
                        png_span = (p0 - 4, r.p, png)
            elif t == T_TEXLIST:
                for _ in range(r.count("texture list", 4)):
                    pointer(0, kinds={T_TEXTURE})
            elif t == T_VERTEX:
                k = r.count("vertex", 16)
                if k:
                    first = struct.unpack("<hhh", r.take(16)[:6])
                    if first == F32_SENTINEL:
                        k -= 1
                        if 22 * k > r.left():
                            raise Refused(f"vertex count {k} larger than the file allows")
                        r.take(22 * k)
                    else:
                        r.take(16 * (k - 1))
                detail = k
                info["vertices"] += k
            elif t == T_DL:
                cmds = []
                for _ in range(r.count("display list", 8)):
                    w0 = r.u32()
                    cmds.append((w0, pointer(0, w0=w0)))
                detail = display_list(n, cmds)
                info["triangles"] += sum({G_TRI1: 1, G_TRI2: 2}.get(w0 >> 24, 0) for w0, _ in cmds)
            elif t == T_GEO:
                words = [pointer(geo) for _ in range(r.count("geo layout", 4))]
                detail = len(geos)
                geos.append({c[0]: c[1:] for c in geo_layout(n, words, names)})
            elif t == T_ANIM:
                r.take(12); r.u32()
                for what in ("animation values", "animation index"):
                    c = r.s32()
                    if c > ARRAY_MAX or 2 * c > r.left():
                        raise Refused(f"{what} count {c} larger than the file allows")
                    r.take(2 * c if c > 0 else 0)
                anims.add(n)
            if n is not None and t != T_ANIM:
                define(n, t, detail)
        if t in (T_ANIM, T_ANIM_TABLE) and not keep_anims:
            info["dropped_anims"] += 1
            continue
        if png_span:
            out += data[start:png_span[0]] + struct.pack("<i", len(png_span[2])) + png_span[2] + data[png_span[1]:r.p]
        else:
            out += data[start:r.p]
        info["nodes"][NODE_NAMES[t]] = info["nodes"].get(NODE_NAMES[t], 0) + 1
    if geos:
        info["geo_commands"] = run_geo(len(geos) - 1, geos)      # DynOS draws the last geo layout
    info["decoded_mb"] = sum(w * h * 4 for w, h in info["pngs"]) / 1048576
    return bytes(out), info


def check_bin(data, stem, actors, names=None):
    names = names or builtin_names()
    if stem not in actors or stem not in names["actors"]:
        raise Refused(f"{stem}: not a curated built-in actor")
    _, info = walk_bin(data, names, keep_anims=True, png_cap=BIN_PNG_MAX)
    if info["nodes"].get("geo_layout", 0) == 0:
        raise Refused(f"{stem}: no geo layout")
    if info["nodes"].get("animation", 0) or info["nodes"].get("animation_table", 0):
        raise Refused(f"{stem}: animation nodes in a staged bin (C4)")
    return info["decoded_mb"]


def _sips_png(png, cap):
    """The PNG re-encoded by sips/magick (resized to cap when larger): the only PNG bytes a staged bin carries."""
    w, h, _, _ = png_info(png)
    with tempfile.TemporaryDirectory(prefix="sm64ar-png-") as d:
        a, b = pathlib.Path(d) / "a.png", pathlib.Path(d) / "b.png"
        a.write_bytes(png)
        _resize_png(a, b, cap if max(w, h) > cap else None)
        return b.read_bytes()


def c4_excluded(pid):
    out = {}
    for src in (VARIANTS[pid]["base"], VARIANTS[pid]["fallback"]) if pid in VARIANTS else ():
        out.update(C4_EXCLUDED.get(src, {}))
    out.update(C4_EXCLUDED.get(pid, {}))
    return out


def prepare_models(pid, entry, with_bowser=False, no_mario=False):
    """extracted/**/<actor>.bin (curated, built-in) -> inflated, validated, anims dropped, every embedded PNG re-encoded by
    sips at <= BIN_PNG_MAX -> prepared/<name>/; built-in raw .tex files copied after check_tex. Writes
    prepared/<name>.expected.json (the staged file names) for verify. Lua and anything else in the pack is never read."""
    names = builtin_names()
    allow = set(CURATED) | (set(BOWSER) if with_bowser else set())
    if no_mario:
        allow -= set(MARIO)
    excluded = c4_excluded(pid)
    allow -= set(excluded)
    out = PACKS / pid / "prepared" / entry["name"]
    for f in (f"{entry['name']}.verified.json", f"{entry['name']}.expected.json"):
        (PACKS / pid / "prepared" / f).unlink(missing_ok=True)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rep = {"staged": [], "skipped": [], "refused": [], "decoded_mb": 0.0, "pngs": 0, "pngs_resized": 0, "anims_dropped": 0,
           "found": {}, "missing": [], "skipped_curated": [],
           "excluded": sorted(excluded.items())}
    picked = {}
    extracted = PACKS / pid / "extracted"
    for p in sorted(extracted.rglob("*.bin")):
        rel = p.relative_to(extracted).as_posix()
        if p.is_symlink() or not p.is_file() or p.stem not in allow or p.stem not in names["actors"]:
            rep["skipped"].append(rel)
            if p.stem in set(CURATED) | set(BOWSER) and p.stem not in excluded:
                rep["skipped_curated"].append(p.stem)
            continue
        if p.stem in picked:
            raise Refused(f"{p.stem}: in two folders ({picked[p.stem].relative_to(extracted)} and {rel})")
        picked[p.stem] = p
    for stem, p in sorted(picked.items()):
        source = p.parent.relative_to(extracted).as_posix()
        rep["found"].setdefault(source, []).append(stem)
        try:
            data, info = walk_bin(inflate_bin(p.read_bytes()), names, png_cap=BIN_PNG_MAX, resize=_sips_png)
            check_bin(data, stem, allow, names)
        except Refused as e:
            rep["refused"].append(f"{stem}: {e}")
            continue
        (out / f"{stem}.bin").write_bytes(data)
        rep["staged"].append(stem)
        rep["decoded_mb"] += info["decoded_mb"]
        rep["pngs"] += len(info["pngs"])
        rep["pngs_resized"] += info["resized"]
        rep["anims_dropped"] += info["dropped_anims"]
    rep["missing"] = sorted(allow - set(picked))
    for t in sorted(extracted.rglob("*.tex")):
        try:
            if t.is_symlink() or not t.is_file():
                raise Refused("not a regular file")
            check_tex(t.read_bytes(), t.stem, names["textures"])
            shutil.copyfile(t, out / t.name)
            rep["staged"].append(t.name)
        except Refused as e:
            rep["refused"].append(f"{t.name}: {e}")
    staged = sorted(f.name for f in out.iterdir())
    (PACKS / pid / "prepared" / f"{entry['name']}.expected.json").write_text(json.dumps(staged, indent=1) + "\n")
    rep["decoded_mb"] = round(rep["decoded_mb"], 1)
    return rep


def expected_staged(pid, entry):
    p = PACKS / pid / "prepared" / f"{entry['name']}.expected.json"
    if not p.is_file():
        raise Refused(f"{p}: missing, so completeness cannot be checked (run: prepare {pid})")
    return {pathlib.Path(n).stem for n in json.loads(p.read_text())}


COSTS = PACKS / "actor_costs.json"
COST_LEVELS = ("bob", "wf", "ccm", "grounds")
BALANCED_BUDGET_VERTS = 170000
MIN_SWAP_INSTANCES = 3


def balanced_pick(base, fallback, cost_base, cost_fallback, instances, budget=BALANCED_BUDGET_VERTS,
                  min_instances=MIN_SWAP_INSTANCES):
    levels = sorted(instances)
    cost = lambda src, a, lv: 0 if src == "vanilla" else (cost_base if src == "base" else cost_fallback).get(a, {}).get(lv, 0)
    choice = {a: "base" if a in base else "fallback" for a in sorted(set(base) | set(fallback))}
    total = lambda lv: sum(cost(s, a, lv) for a, s in choice.items())
    swap_to = lambda a: "fallback" if choice[a] == "base" and a in fallback else "vanilla"
    while True:
        over = [lv for lv in levels if total(lv) > budget]
        saves = [(max(cost(s, a, lv) - cost(swap_to(a), a, lv) for lv in many), a) for a, s in choice.items()
                 if (s == "base" or (s == "fallback" and a not in base))
                 and (many := [lv for lv in over if instances[lv].get(a, 0) >= min_instances])]
        saves = [(s, a) for s, a in saves if s > 0]
        if not saves:
            return choice, {lv: total(lv) for lv in levels}, over
        _, a = min(saves, key=lambda x: (-x[0], x[1]))
        choice[a] = swap_to(a)


def verified_files(pid, entry):
    out = PACKS / pid / "prepared" / entry["name"]
    v = PACKS / pid / "prepared" / f"{entry['name']}.verified.json"
    if not v.is_file():
        raise Refused(f"{entry['name']}: not verified (run: prepare {pid})")
    want = json.loads(v.read_text())["files"]
    files = {}
    for f in sorted(out.iterdir()) if out.is_dir() else ():
        if f.is_symlink() or not f.is_file() or f.name not in want:
            raise Refused(f"{f}: not a verified file of {entry['name']}")
        data = f.read_bytes()
        if hashlib.sha256(data).hexdigest() != want[f.name]:
            raise Refused(f"{f}: does not match its pinned hash (re-run: prepare {pid})")
        files[f.name] = data
    if sorted(files) != sorted(want):
        raise Refused(f"{entry['name']}: verified files missing")
    return files


def tex_actor(tex_name, actors):
    hits = [a for a in actors if a.endswith("_geo") and tex_name.startswith(a[:-len("_geo")] + "_seg")]
    return max(hits, key=len) if hits else None


def check_variant_rule(pid, entry):
    b = PACKS / pid / "prepared" / f"{entry['name']}.balanced.json"
    rep = json.loads(b.read_text()) if b.is_file() else {}
    if (rep.get("budget"), rep.get("min_instances")) != (BALANCED_BUDGET_VERTS, MIN_SWAP_INSTANCES):
        raise Refused(f"{entry['name']}: prepared with budget {rep.get('budget')} / min {rep.get('min_instances')}, the rule is "
                      f"now {BALANCED_BUDGET_VERTS} / {MIN_SWAP_INSTANCES} (re-run: prepare {pid})")


def prepare_variant(pid, entry, manifest, costs_path=None):
    v = VARIANTS[pid]
    src = {k: manifest[v[k]] for k in ("base", "fallback")}
    files = {k: verified_files(v[k], src[k]) for k in ("base", "fallback")}
    costs_path = pathlib.Path(costs_path or COSTS)
    if not costs_path.is_file():
        raise Refused(f"{costs_path}: missing (run: python3 scripts/pack_costs.py)")
    costs = json.loads(costs_path.read_text())
    for k in ("base", "fallback"):
        pin = hashlib.sha256((PACKS / v[k] / "prepared" / f"{src[k]['name']}.verified.json").read_bytes()).hexdigest()
        if costs.get("sources", {}).get(src[k]["name"]) != pin:
            raise Refused(f"{costs_path}: measured on other {src[k]['name']} files (re-run: python3 scripts/pack_costs.py)")
    if sorted(costs["instances"]) != sorted(COST_LEVELS):
        raise Refused(f"{costs_path}: levels {sorted(costs['instances'])}, want {sorted(COST_LEVELS)}")
    bins = {k: {pathlib.Path(n).stem for n in files[k] if n.endswith(".bin")} for k in files}
    excluded = c4_excluded(pid)
    choice, totals, over = balanced_pick(bins["base"] - set(excluded), bins["fallback"] - set(excluded),
                                         costs["extra"].get(src["base"]["name"], {}),
                                         costs["extra"].get(src["fallback"]["name"], {}), costs["instances"],
                                         BALANCED_BUDGET_VERTS, MIN_SWAP_INSTANCES)
    out = PACKS / pid / "prepared" / entry["name"]
    for f in (f"{entry['name']}.verified.json", f"{entry['name']}.expected.json", f"{entry['name']}.balanced.json"):
        (PACKS / pid / "prepared" / f).unlink(missing_ok=True)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for actor, s in sorted(choice.items()):
        if s != "vanilla":
            (out / f"{actor}.bin").write_bytes(files[s][f"{actor}.bin"])
    dropped_tex = []
    for name, data in sorted(files["base"].items()):
        if name.endswith(".tex"):
            if (a := tex_actor(name, choice)) and choice[a] != "base":
                dropped_tex.append(name)
                continue
            (out / name).write_bytes(data)
    staged = sorted(f.name for f in out.iterdir())
    (PACKS / pid / "prepared" / f"{entry['name']}.expected.json").write_text(json.dumps(staged, indent=1) + "\n")
    measured = {a for c in (costs["extra"].get(src["base"]["name"], {}), costs["extra"].get(src["fallback"]["name"], {}))
                for a, per in c.items() if per}
    rep = {"choice": choice, "totals": totals, "over": over, "budget": BALANCED_BUDGET_VERTS,
           "min_instances": MIN_SWAP_INSTANCES, "sources": {k: src[k]["name"] for k in src},
           "unmeasured": sorted(a for a in choice if a not in measured), "dropped_tex": dropped_tex}
    (PACKS / pid / "prepared" / f"{entry['name']}.balanced.json").write_text(json.dumps(rep, indent=1, sort_keys=True) + "\n")
    return rep


def stage_harness(save):
    dest = pathlib.Path(save) / "dynos" / "packs"
    dest.mkdir(parents=True, exist_ok=True)
    n = 0
    for v in sorted(PACKS.glob("*/prepared/*.verified.json")):
        name = v.name[:-len(".verified.json")]
        link = dest / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(v.parent / name, target_is_directory=True)
        n += 1
    return n



def report(packs=PACKS, out=print):
    total = 0.0
    for v in sorted(pathlib.Path(packs).glob("*/prepared/*.verified.json")):
        d = json.loads(v.read_text())
        total += d["decoded_mb"]
        out(f"{d['name']}: {len(d['files'])} files, decoded_mb={d['decoded_mb']}")
    out(f"total decoded_mb={round(total, 1)}")
    return total


def main(argv):
    if not argv or argv[0] not in ("pin-from-sources", "list", "fetch", "prepare", "verify", "stage-harness", "report"):
        print(__doc__)
        return 2
    if argv[0] == "report":
        report()
        return 0
    if argv[0] == "pin-from-sources":
        m = pin_from_sources()
        print(f"pinned {len(m)} packs into {MANIFEST} (local only)")
        return 0
    if argv[0] == "stage-harness":
        if len(argv) != 2:
            print(__doc__)
            return 2
        print(f"staged {stage_harness(argv[1])} packs into {argv[1]}/dynos/packs")
        return 0
    m = load_manifest()
    if argv[0] == "list":
        for pid, e in sorted(m.items()):
            prepared = PACKS / pid / "prepared" / e["name"]
            size = f"variant of {e['base']} + {e['fallback']}" if e.get("variant") else f"{e['bytes']} bytes"
            print(f"{pid}: {e['kind']} '{e['name']}' {size}, prepared={'yes' if prepared.is_dir() else 'no'}")
        return 0
    flags = argv[2:]
    if len(argv) < 2 or argv[1] not in m:
        print(f"unknown pack id (pinned: {', '.join(sorted(m)) or 'none: run pin-from-sources'})")
        return 2
    model = m[argv[1]]["kind"] == "models"
    variant = bool(m[argv[1]].get("variant"))
    if any(f not in ("--with-bowser", "--no-mario") for f in flags) or (flags and not (argv[0] == "prepare" and model and not variant)):
        print(__doc__)
        return 2
    if argv[0] in ("prepare", "verify"):
        e = m[argv[1]]
        try:
            table = builtin_textures()
            symbols = {x[0] for x in table.values()}
            if variant:
                rep = prepare_variant(argv[1], e, m) if argv[0] == "prepare" else None
                v = verify(argv[1], e, symbols, actors=set(CURATED) | set(BOWSER), expected=expected_staged(argv[1], e))
            elif model:
                rep = prepare_models(argv[1], e, "--with-bowser" in flags, "--no-mario" in flags) if argv[0] == "prepare" else None
                v = verify(argv[1], e, symbols, actors=set(CURATED) | set(BOWSER), expected=expected_staged(argv[1], e))
            else:
                rep = prepare_textures(argv[1], e) if argv[0] == "prepare" else None
                v = verify(argv[1], e, symbols, expected=expected_staged(argv[1], e))
        except (Refused, subprocess.CalledProcessError, OSError) as ex:
            print(f"refused: {ex}")
            return 1
        if rep and variant:
            for s in ("base", "fallback", "vanilla"):
                actors = sorted(a for a, c in rep["choice"].items() if c == s)
                print(f"{argv[1]}: {s} ({rep['sources'].get(s, 'built-in')}): {len(actors)}" + (f" ({', '.join(actors)})" if s != "base" else ""))
            print(f"{argv[1]}: added vertices per level (budget {rep['budget']}): "
                  + ", ".join(f"{lv} {n}" for lv, n in sorted(rep["totals"].items())))
            for lv in rep["over"]:
                print(f"  over budget: {lv} (no eligible actor left to swap)")
            if rep["unmeasured"]:
                print(f"{argv[1]}: unmeasured (cost 0, model kept): {', '.join(rep['unmeasured'])}")
            if rep["dropped_tex"]:
                print(f"{argv[1]}: base .tex dropped (their actor is swapped): {', '.join(rep['dropped_tex'])}")
        elif rep and model:
            for source, stems in sorted(rep["found"].items()):
                print(f"{argv[1]}: curated found in {source}: {len(stems)}")
            print(f"{argv[1]}: curated missing: {len(rep['missing'])}" + (f" ({', '.join(rep['missing'])})" if rep["missing"] else ""))
            for actor, why in rep["excluded"]:
                print(f"{argv[1]}: {actor} excluded: {why}")
            if rep["skipped_curated"]:
                print(f"{argv[1]}: curated skipped by flags: {', '.join(sorted(rep['skipped_curated']))}")
            print(f"{argv[1]}: staged={len(rep['staged'])} skipped={len(rep['skipped'])} refused={len(rep['refused'])} "
                  f"pngs={rep['pngs']} pngs_resized={rep['pngs_resized']} anims_dropped={rep['anims_dropped']} "
                  f"decoded_mb={rep['decoded_mb']}")
            for r in rep["refused"]:
                print(f"  refused: {r}")
        elif rep:
            print(f"{argv[1]}: matched={rep['matched']} dropped={rep['dropped']} resized={rep['resized']} "
                  f"unmatched={len(rep['unmatched'])} refused={len(rep['refused'])}")
            for r in rep["refused"]:
                print(f"  refused: {r}")
        print(f"{argv[1]}: {v['files']} {'.bin/.tex' if model else '.tex'} files, decoded_mb={v['decoded_mb']}")
        return 0
    if variant:
        print(f"{argv[1]}: a prepared variant has nothing to fetch (fetch and prepare {m[argv[1]]['base']} and {m[argv[1]]['fallback']})")
        return 2
    try:
        archive = fetch(m[argv[1]], PACKS / argv[1])
        r = safe_extract(archive, PACKS / argv[1] / "extracted", print)
    except Refused as e:
        print(f"refused: {e}")
        return 1
    print(f"{argv[1]}: {r['files']} files, {r['bytes']} bytes extracted; {len(r['skipped'])} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
