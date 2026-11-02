#!/usr/bin/env python3
"""scripts/guided_build.py — what ./build.py runs. It checks the Mac, asks what it needs (only on a terminal; every answer
is also a flag and is remembered in local/build.json), then runs the whole chain and installs or launches Diorama64.

  ./build.py                                   # asks on the first run, then reuses the answers
  ./build.py --reconfigure                     # asks again
  ./build.py --rom PATH --target simulator     # no questions
  ./build.py --target headset --team TEAMID --device "NAME"
  ./build.py --packs all                       # the known packs from their official downloads (all, none, 1,3 or ids)
  ./build.py --pack ID=PATH_OR_URL             # a known pack from an archive or URL of your own (repeatable)

Your ROM and the chosen packs always go inside the app.
"""
import argparse, collections, hashlib, json, os, pathlib, re, subprocess, sys, tempfile
import sm64ar_common as common
from rom_to_z64 import expected_sha1, to_z64

ROOT = common.ROOT
LOCAL = ROOT / "local"
CONFIG = LOCAL / "build.json"
INSTALLED = LOCAL / "installed.json"
OFFICIAL = "official"
CONFIG_TYPES = {"target": str, "packs": dict, "team": str, "bundle_id": str, "device": str, "device_name": str}
ROM = LOCAL / "baserom.us.z64"
PACKS = LOCAL / "packs"
SCRIPTS = ROOT / "scripts"
SIM_OUT = LOCAL / "sim"
TARGETS = ("simulator", "headset")
MAX_PACK_BYTES = 1 << 30
HOMEBREW = ("Homebrew is not installed. Install it as https://brew.sh says, then run ./build.py again:\n"
            '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"')


class Stop(Exception):
    """A problem the user fixes: printed as one error message, no traceback."""



class Asker:

    def __init__(self, interactive, read=input, write=lambda *t: print(*t, flush=True)):
        self.interactive, self.read, self.write = interactive, read, write

    def _line(self, prompt):
        try:
            return self.read(prompt).strip()
        except EOFError:
            raise Stop("cancelled") from None

    def text(self, question, flag, default=None):
        if not self.interactive:
            raise Stop(f"{question}: no terminal to ask on; pass {flag}")
        while True:
            a = self._line(f"{question}" + (f" [{default}]" if default else "") + ": ")
            if a:
                return a
            if default:
                return default

    def optional(self, question):
        return self._line(f"{question}: ") if self.interactive else ""

    def yes(self, question, default=False):
        if not self.interactive:
            return default
        while True:
            a = self._line(f"{question} [{'Y/n' if default else 'y/N'}] ").lower()
            if not a:
                return default
            if a in ("y", "yes", "n", "no"):
                return a[0] == "y"

    def choose(self, question, options, flag, default=1):
        if not self.interactive:
            raise Stop(f"{question}: no terminal to ask on; pass {flag}")
        for i, o in enumerate(options, 1):
            self.write(f"  {i}) {o}")
        while True:
            a = self.text(question, flag, str(default))
            if a.isdigit() and 1 <= int(a) <= len(options):
                return int(a) - 1
            self.write(f"Type a number from 1 to {len(options)}.")


def clean_path(text):
    t = (text or "").strip()
    if len(t) >= 2 and t[0] == t[-1] and t[0] in "'\"":
        t = t[1:-1]
    else:
        t = re.sub(r"\\(.)", r"\1", t)
    return pathlib.Path(os.path.expanduser(t)).resolve() if t else None


def clean_source(text):
    t = (text or "").strip().strip("'\"")
    if t.startswith("https://"):
        return t
    if re.match(r"[a-z]+://", t):
        raise Stop(f"{t}: only https URLs or a file on this Mac")
    p = clean_path(text)
    if p is None:
        raise Stop("no pack archive given")
    return str(p)


def parse_packs_spec(spec, known):
    ids, words = list(known), (spec or "").replace(",", " ").split()
    if [w.lower() for w in words] in ([], ["none"], ["0"]):
        return []
    if [w.lower() for w in words] in (["all"], ["a"]):
        return ids
    out = []
    for w in words:
        pid = ids[int(w) - 1] if w.isdigit() and 1 <= int(w) <= len(ids) else w if w in known else None
        if pid is None:
            raise Stop(f"packs {spec!r}: {w!r} is not 1 to {len(ids)} or a known id ({', '.join(ids)}); or all, none")
        if pid not in out:
            out.append(pid)
    return out


def parse_pack(text, known):
    pid, sep, src = (text or "").partition("=")
    pid = pid.strip()
    if not sep or not src.strip():
        raise Stop(f"--pack {text!r}: use ID=PATH_OR_URL")
    if pid not in known:
        raise Stop(f"--pack {pid!r}: unknown id (known: {', '.join(sorted(known))})")
    return pid, clean_source(src)


def read_json(path, what):
    path = pathlib.Path(path)
    if not path.exists():
        return {}
    try:
        c = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        raise Stop(f"{path} is damaged ({e}): delete it and run ./build.py again ({what})") from None
    if not isinstance(c, dict):
        raise Stop(f"{path} is damaged (not a JSON object): delete it and run ./build.py again ({what})")
    return c


def load_config(path=None):
    path = path or CONFIG
    c = read_json(path, "it holds your answers")
    for k, v in c.items():
        bad = k not in CONFIG_TYPES or not isinstance(v, CONFIG_TYPES[k]) or \
            (k == "packs" and not all(isinstance(i, str) and isinstance(s, str) and s for i, s in v.items()))
        if bad:
            raise Stop(f"{path} is damaged (its {k!r} is not valid): delete it, or run ./build.py --reconfigure after "
                       "deleting it")
    return c


def save_config(config, path=None):
    path = pathlib.Path(path or CONFIG)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, indent=2, sort_keys=True) + "\n")


def default_bundle_id(team):
    return f"dev.{team.lower()}.diorama64"                                            # the team id makes it unique



def apple_silicon(run=subprocess.run):
    r = run(["sysctl", "-n", "hw.optional.arm64"], capture_output=True, text=True)
    return r.returncode == 0 and r.stdout.strip() == "1"


def brew_step(asker, brew=None, run=subprocess.run):
    """Missing formulae: show the exact command and run it only after a yes on a terminal."""
    brew = brew or common.find_brew()
    if not brew:
        raise Stop(HOMEBREW)
    try:
        missing = common.missing_formulae(brew=brew)
    except RuntimeError:
        raise Stop(HOMEBREW) from None
    if not missing:
        return []
    cmd = [brew, "install", *missing]
    asker.write(f"Missing Homebrew formulae: {' '.join(missing)}. To install them:\n  brew install {' '.join(missing)}")
    if not asker.yes("Run it now?"):
        raise Stop(f"missing Homebrew formulae; run: brew install {' '.join(missing)}")
    if run(cmd).returncode:
        raise Stop(f"brew install {' '.join(missing)} failed")
    return missing


def submodule_ready(path, root=ROOT):
    r = subprocess.run(["git", "-C", str(root), "submodule", "status", "--", path], capture_output=True, text=True)
    return r.returncode == 0 and r.stdout[:1] == " "


def ensure_submodule(path, write=print, root=ROOT):
    if submodule_ready(path, root):
        return False
    write(f"Fetching the submodule {path} (once) ...")
    r = subprocess.run(["git", "-C", str(root), "submodule", "update", "--init", "--", path])
    if r.returncode or not submodule_ready(path, root):
        raise Stop(f"could not check out {path}: run git submodule update --init {path}")
    return True


def git_checkout(root=ROOT):
    r = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    return r.returncode == 0 and pathlib.Path(r.stdout.strip()).resolve() == pathlib.Path(root).resolve()



def us_sha1(upstream=common.UPSTREAM, pin=common.PIN):
    mine = expected_sha1((LOCAL / "baserom.us.sha1",))
    if mine:
        return mine
    r = subprocess.run(["git", "-C", str(upstream), "show", f"{pin}:sm64.us.sha1"], capture_output=True, text=True)
    words = r.stdout.split() if r.returncode == 0 else []
    if words and re.fullmatch(r"[0-9a-fA-F]{40}", words[0]):
        return words[0].lower()
    return expected_sha1()


def rom_ok(path, want):
    path = pathlib.Path(path)
    return path.is_file() and hashlib.sha1(path.read_bytes()).hexdigest() == want


def import_rom(src, dst, want, what="Super Mario 64 (US)"):
    try:
        data = pathlib.Path(src).read_bytes()
    except OSError as e:
        raise Stop(f"cannot read {src}: {e.strerror}") from None
    try:
        z = to_z64(data)
    except ValueError as e:
        raise Stop(f"{src}: {e}") from None
    got = hashlib.sha1(z).hexdigest()
    if got != want:
        raise Stop(f"{src} is not the {what} ROM (SHA-1 {got}, expected {want}); nothing written")
    dst = pathlib.Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".part")
    tmp.write_bytes(z)
    os.replace(tmp, dst)
    return got



IDENTITY = re.compile(r'^\s*\d+\)\s+([0-9A-F]{40})\s+"(Apple Development: [^"]*)"', re.M)
CERT = re.compile(r"SHA-1 hash: ([0-9A-F]{40}).*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", re.S)


def parse_identities(text):
    return IDENTITY.findall(text)


def parse_certificates(text):
    return {m.group(1): m.group(2) for m in CERT.finditer(text)}


def team_of_subject(subject):
    m = re.search(r"(?:^|[,/\s])OU\s*=\s*([A-Z0-9]{10})(?=[,/\s]|$)", subject)
    return m.group(1) if m else None


def find_teams(run=subprocess.run):
    ids = parse_identities(run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True,
                               text=True).stdout)
    if not ids:
        return []
    pems = parse_certificates(run(["security", "find-certificate", "-a", "-c", "Apple Development", "-Z", "-p"],
                                  capture_output=True, text=True).stdout)
    out, seen = [], set()
    for sha, name in ids:
        if sha not in pems:
            continue
        subj = run(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"], input=pems[sha],
                   capture_output=True, text=True).stdout
        team = team_of_subject(subj)
        if team and team not in seen:
            seen.add(team)
            out.append((name, team))
    return out


Headset = collections.namedtuple("Headset", "name ident available")


def parse_devices(listing):
    """The headsets, reachable ones first. Names need not be unique, so installs go by identifier."""
    out = []
    for d in (listing.get("result") or {}).get("devices") or []:
        hw, name = d.get("hardwareProperties") or {}, (d.get("deviceProperties") or {}).get("name")
        if name and (hw.get("platform") in ("xrOS", "visionOS") or str(hw.get("productType", "")).startswith("RealityDevice")
                     or "vision" in str(hw.get("deviceType", "")).lower()):
            tunnel = (d.get("connectionProperties") or {}).get("tunnelState")
            out.append(Headset(name, d.get("identifier") or name, tunnel != "unavailable"))
    return sorted(out, key=lambda h: not h.available)


def headset_labels(found):
    names = [h.name for h in found]
    return [h.name + ("" if h.available else " (not reachable)") + (f" [{h.ident[:8]}]" if names.count(h.name) > 1 else "")
            for h in found]


def list_devices():
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "devices.json"
        r = subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", str(out)], env=common.env(),
                           capture_output=True, text=True)
        if r.returncode or not out.is_file():
            raise Stop(f"xcrun devicectl list devices failed: {(r.stderr or r.stdout).strip()}")
        return parse_devices(json.loads(out.read_text()))



def known_packs():
    import packs
    return packs.KNOWN


def pack_line(pid, known):
    kind, name = known[pid]
    return f"{pid}: {'HD textures' if kind == 'textures' else '3D models'} ({name})"


PACKS_QUESTION = "Packs to add (numbers, 0 for none, Enter for all)"


def packs_menu(known):
    return [f"  {i}) {known[pid][1]} ({'HD textures' if known[pid][0] == 'textures' else '3D models'})"
            for i, pid in enumerate(known, 1)]


NO_CERTIFICATE = """No Apple Development certificate found. In Xcode:
  - Xcode > Settings > Apple Accounts: + > Apple Account, sign in
  - select your team, then Manage Certificates… > + > Apple Development
Then run ./build.py again."""


NO_HEADSET = """No paired Apple Vision Pro found. With the Mac and the headset on the same Wi-Fi:
  1. On the headset: Settings > General > Remote Devices, and stay on that screen
  2. In Xcode: Window > Devices and Simulators > Devices, pick the Apple Vision Pro from the list on the left (don't
     press +), Pair, and enter the code the headset shows
  3. On the headset: Settings > Privacy & Security > Developer Mode on (the headset restarts)
Then run ./build.py again."""


LINUX_ONLY_FLAGS = (("team", "--team"), ("bundle_id", "--bundle-id"), ("device", "--device"), ("reinstall", "--reinstall"))


def resolve(a, saved, asker, teams=find_teams, devices=list_devices, platform=sys.platform):
    from visionos_device import valid_bundle_id, valid_team
    linux = platform != "darwin"
    if linux:
        if a.target:
            raise Stop("on Linux ./build.py builds for Meta Quest")
        for attr, flag in LINUX_ONLY_FLAGS:
            if getattr(a, attr, None):
                raise Stop(f"{flag} is for the Apple Vision Pro; on Linux ./build.py builds for Meta Quest")
    known = known_packs()
    c = dict(saved)
    ask = lambda k: a.reconfigure or k not in saved                                    # noqa: E731
    c["target"] = "quest" if linux else a.target or saved.get("target", "headset")     # simulator: --target only
    if not linux and c["target"] not in TARGETS:
        raise Stop(f"target {c['target']!r}: simulator or headset")
    if a.packs is not None or a.no_packs or a.pack:
        if a.no_packs:
            c["packs"] = {}
        elif a.packs is not None:
            c["packs"] = {pid: OFFICIAL for pid in parse_packs_spec(a.packs, known)}
        else:
            c["packs"] = dict(saved.get("packs") or {})
        c["packs"].update(parse_pack(p, known) for p in a.pack)
    elif ask("packs"):
        answer = ""
        if asker.interactive:
            for line in packs_menu(known):
                asker.write(line)
            answer = asker.optional(PACKS_QUESTION).strip() or "all"
        c["packs"] = {pid: OFFICIAL for pid in parse_packs_spec(answer, known)}
    if linux:
        return c
    if c["target"] == "headset":
        if a.team:
            c["team"] = a.team
        elif ask("team"):
            found = teams()
            if found:
                opts = [f"{team} ({name})" for name, team in found] + ["another team (type its id)"]
                i = asker.choose("Signing team", opts, "--team TEAMID")
                c["team"] = found[i][1] if i < len(found) else asker.text("Team id", "--team TEAMID")
            else:
                raise Stop(NO_CERTIFICATE)
        if not valid_team(c.get("team")):
            raise Stop(f"team {c.get('team')!r}: 10 characters, A-Z 0-9")
        if a.bundle_id:
            c["bundle_id"] = a.bundle_id
        elif not c.get("bundle_id"):
            c["bundle_id"] = default_bundle_id(c["team"])
        if not valid_bundle_id(c.get("bundle_id")):
            raise Stop(f"bundle id {c.get('bundle_id')!r}: like com.yourname.diorama64")
        if a.device:
            c["device"] = a.device
            c.pop("device_name", None)
            if not a.no_install:
                pick_by_name(c, devices(), asker)
        elif not a.no_install and ask("device"):
            found = devices()
            if not found:
                raise Stop(NO_HEADSET)
            h = found[asker.choose("Headset", headset_labels(found), '--device "NAME"')]
            c["device"], c["device_name"] = h.ident, h.name
        elif not a.no_install and c.get("device"):
            pick_by_name(c, devices(), asker)
    return c


def pick_by_name(c, found, asker):
    """A device given or saved by name becomes its identifier; a name several headsets share is asked about."""
    if any(h.ident == c["device"] for h in found):
        return
    same = [h for h in found if h.name == c["device"]]
    if len(same) == 1:
        c["device"], c["device_name"] = same[0].ident, same[0].name
    elif same:
        if not asker.interactive:
            raise Stop(f"several headsets are called {c['device']!r}: pass --device with one of "
                       + ", ".join(h.ident for h in same))
        h = same[asker.choose("Headset", headset_labels(same), "--device IDENTIFIER")]
        c["device"], c["device_name"] = h.ident, h.name



def step(script, *args, isolated=False, write=print, env=None):
    cmd = [sys.executable, *(["-I"] if isolated else []), str(SCRIPTS / script), *map(str, args)]
    write("+ " + " ".join(cmd[1:]))
    if subprocess.run(cmd, env={**common.env(), **(env or {})}).returncode:
        raise Stop(f"{script} failed (see above)")


def sources_row(pid, src, size, sha):
    if "|" in src:
        raise Stop(f"{src}: a source cannot contain |")
    return f"| {pid} | {src} | {size} | {sha} |"


def write_sources_row(pid, row, path=None):
    path = pathlib.Path(path or PACKS / "SOURCES.md")
    lines = path.read_text().splitlines() if path.is_file() else ["| id | url | bytes | sha256 |", "|---|---|---|---|"]
    lines = [l for l in lines if [c.strip() for c in l.strip().strip("|").split("|")][:1] != [pid]] + ([row] if row else [])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def use_official(pid, packs_dir=None):
    packs_dir = pathlib.Path(packs_dir or PACKS)
    if (packs_dir / "SOURCES.md").is_file():
        write_sources_row(pid, None, packs_dir / "SOURCES.md")
    m = read_json(packs_dir / "manifest.json", "pinned packs; packs.py pin-from-sources writes it again")
    if pid in m:
        del m[pid]
        (packs_dir / "manifest.json").write_text(json.dumps(m, indent=2, sort_keys=True) + "\n")


def hash_file(p):
    h, n = hashlib.sha256(), 0
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
            n += len(b)
    return n, h.hexdigest()


def download(url, dst, opener=None):
    """https only (redirects too), size-capped, to dst; the hash is taken by the caller (first download)."""
    import packs
    if not url.startswith("https://"):
        raise Stop(f"{url}: only https downloads")
    dst.parent.mkdir(parents=True, exist_ok=True)
    part, n = dst.with_name(dst.name + ".part"), 0
    try:
        with (opener or packs.https_open)(url, timeout=60) as r, open(part, "wb") as f:
            for b in iter(lambda: r.read(1 << 20), b""):
                n += len(b)
                if n > MAX_PACK_BYTES:
                    raise Stop(f"{url}: larger than {MAX_PACK_BYTES} bytes")
                f.write(b)
        part.rename(dst)
    except packs.Refused as e:
        raise Stop(str(e)) from None
    except OSError as e:
        raise Stop(f"{url}: download failed ({e})") from None
    finally:
        part.unlink(missing_ok=True)


def own_url_archive(pid, src):
    import packs
    name = packs.url_file_name(pid, src)
    if not packs._file_ok(name):
        raise Stop(f"{src}: the URL does not end in a plain file name; download it and give its path")
    return PACKS / pid / name


def pin_pack(pid, src, opener=None, write=print):
    if src.startswith("https://"):
        dst = own_url_archive(pid, src)
        if not dst.is_file():
            write(f"Downloading {src} ...")
            download(src, dst, opener)
        size, sha = hash_file(dst)
    else:
        p = pathlib.Path(src)
        if not p.is_file():
            raise Stop(f"{src}: no such file")
        size, sha = hash_file(p)
        if size > MAX_PACK_BYTES:
            raise Stop(f"{src}: larger than {MAX_PACK_BYTES} bytes")
    write_sources_row(pid, sources_row(pid, src, size, sha))
    return sha


def source_sha(pid, src):
    import packs
    if src == OFFICIAL:
        return packs.OFFICIAL[pid][2]
    if src.startswith("https://"):
        dst = own_url_archive(pid, src)
        return hash_file(dst)[1] if dst.is_file() else None
    if not pathlib.Path(src).is_file():
        raise Stop(f"pack {pid}: {src}: no such file")
    return hash_file(src)[1]


def stamp_ok(pid, name, sha, packs_dir=None):
    packs_dir = pathlib.Path(packs_dir or PACKS)
    stamp = read_json(packs_dir / pid / ".build-stamp", "a prepared pack's stamp; it is written again")
    if stamp and not (isinstance(stamp.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", stamp["sha256"])):
        raise Stop(f"{packs_dir / pid / '.build-stamp'} is damaged: delete it and run ./build.py again")
    return bool(sha) and stamp.get("sha256") == sha and (packs_dir / pid / "prepared" / f"{name}.verified.json").is_file()


def write_stamp(pid, sha, packs_dir=None):
    (pathlib.Path(packs_dir or PACKS) / pid / ".build-stamp").write_text(json.dumps({"sha256": sha}) + "\n")


HARNESS = ROOT / "build" / "macos" / "harness" / "bin" / "sm64ar_harness"


def pack_tool(run=None):
    """Linux: the host pack tool that stands in for the macOS harness (generates the .tex files)."""
    run = run or common.run
    b = ROOT / "build" / "host"
    if not (b / "CMakeCache.txt").is_file() and run(["cmake", "-S", ROOT, "-B", b, "-G", "Ninja", "-DSM64AR_SM64=ON"],
                                                    check=False):
        raise Stop(f"configuring the host build in {b} failed (see above); the packs need the pack tool")
    if run(["cmake", "--build", b, "--target", "sm64ar_pack_tool"], check=False):
        raise Stop(f"building the pack tool in {b} failed (see above); the packs need it")


def harness(run=None):
    if not common.IS_MAC:
        return pack_tool(run)
    run = run or common.run
    b = ROOT / "build" / "macos"
    if not (b / "CMakeCache.txt").is_file() and run(["cmake", "-S", ROOT, "-B", b, "-G", "Ninja"], check=False):
        raise Stop(f"configuring the macOS harness in {b} failed (see above); the packs need it")
    if run(["cmake", "--build", b, "--target", "sm64ar_harness"], check=False):
        raise Stop(f"building the macOS harness in {b} failed (see above); the packs need it")


def prepare_packs(chosen, write=print):
    import packs
    known = known_packs()
    names = []
    for pid, src in sorted(chosen.items()):
        name = known[pid][1]
        names.append(name)
        sha = source_sha(pid, src)
        if stamp_ok(pid, name, sha):
            write(f"pack {pid} up to date")
            continue
        if src == OFFICIAL:
            use_official(pid)
        else:
            sha = pin_pack(pid, src, write=write)
            step("packs.py", "pin-from-sources", isolated=True, write=write)
        step("packs.py", "fetch", pid, isolated=True, write=write)
        if known[pid][0] == "textures":
            harness()
        step("packs.py", "prepare", pid, isolated=True, write=write)
        write_stamp(pid, sha)
    for vid, v in packs.VARIANTS.items():
        if v["base"] in chosen and v["fallback"] in chosen:
            names.append(v["name"])
            key = hashlib.sha256(b"".join((PACKS / v[k] / "prepared" / f"{known[v[k]][1]}.verified.json").read_bytes()
                                          for k in ("base", "fallback"))).hexdigest()
            if stamp_ok(vid, v["name"], key):
                write(f"pack {vid} up to date")
                continue
            if not common.IS_MAC:
                write(f"pack {vid} skipped: its costs are measured with the macOS harness")
                names.remove(v["name"])
                continue
            harness()
            step("pack_costs.py", write=write, env={"SM64AR_HARNESS": str(HARNESS)})
            step("packs.py", "prepare", vid, isolated=True, write=write)
            write_stamp(vid, key)
    return names


def app_digest(app, run=subprocess.run):
    """The installed-build key: the app's CDHash (it covers the code, Info.plist and every resource, Bundled/ too)."""
    r = run(["codesign", "-dvvv", str(app)], capture_output=True, text=True)
    m = re.search(r"^CDHash=([0-9a-f]+)$", (r.stderr or "") + (r.stdout or ""), re.M)
    if not m:
        raise Stop(f"cannot read the signature of {app}")
    return m.group(1)


def install(device, bundle_id, write=print, digest=None, again=False, label=None):
    from visionos_build import BUILD_DEVICE, find_app
    try:
        app = find_app(BUILD_DEVICE, "Release", device=True)
    except RuntimeError as e:
        raise Stop(str(e)) from None
    digest = digest or app_digest(app)
    key = f"{device}|{bundle_id}"
    done = read_json(INSTALLED, "what was installed where")
    if done.get(key) == digest and not again:
        write(f"{label or device} already has this build (if you deleted the app there, run ./build.py --reinstall).")
        return False
    step("visionos_device.py", "install", "--device", device, write=write)
    done[key] = digest
    INSTALLED.write_text(json.dumps(done, indent=2, sort_keys=True) + "\n")
    return True



def parse_args(argv):
    ap = argparse.ArgumentParser(prog="./build.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", help="your Super Mario 64 (US) ROM, any byte order")
    ap.add_argument("--target", choices=TARGETS)
    ap.add_argument("--packs", metavar="all|none|1,3|IDS", help="the known packs from their official downloads")
    ap.add_argument("--pack", action="append", default=[], metavar="ID=PATH_OR_URL",
                    help="a known pack from an archive or https URL of your own (repeatable)")
    ap.add_argument("--no-packs", action="store_true", help="no packs (as --packs none)")
    ap.add_argument("--team", help="headset: your Apple development team id")
    ap.add_argument("--bundle-id", help="headset: default dev.<team>.diorama64")
    ap.add_argument("--device", help="headset: its name (xcrun devicectl list devices)")
    ap.add_argument("--no-install", action="store_true", help="headset: build only")
    ap.add_argument("--reinstall", action="store_true", help="headset: install even if it already has this build")
    ap.add_argument("--no-launch", action="store_true", help="simulator, Quest: build/install without starting the app")
    ap.add_argument("--reconfigure", action="store_true", help="ask every question again")
    return ap.parse_args(argv)


def quest_main(a, c, w):
    import quest_device
    if not a.no_install:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True, env=common.env())
        if quest_device.find_device(r.stdout if r.returncode == 0 else "") is None:
            raise Stop("connect one Quest with USB debugging allowed")
    for s in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        step(s, write=w)
    names = prepare_packs(c.get("packs") or {}, w)
    data = ["--packs", ",".join(names)] if names else ["--no-packs"]
    flags = ["--no-install"] if a.no_install else [] if a.no_launch else ["--launch"]
    step("quest_build.py", "--config", "Release", *data, *flags, write=w)
    w("Built (not installed: --no-install)." if a.no_install else "Diorama64 is on the Quest.")
    return 0


def main(argv, asker=None):
    a = parse_args(argv)
    asker = asker or Asker(sys.stdin.isatty())
    w = asker.write
    linux = sys.platform != "darwin"
    if linux:
        for attr, flag in LINUX_ONLY_FLAGS:
            if getattr(a, attr, None):
                raise Stop(f"{flag} is for the Apple Vision Pro; on Linux ./build.py builds for Meta Quest")
        if a.target:
            raise Stop("on Linux ./build.py builds for Meta Quest")
        missing = common.tool_problems()
        if missing:
            raise Stop(f"missing tools: {', '.join(missing)}; run inside the nix dev shell: direnv allow, or nix develop")
    elif not apple_silicon():
        raise Stop("Diorama64 builds on a Mac with Apple silicon")
    if not git_checkout():
        raise Stop(f"{ROOT} is not a git checkout: git clone https://github.com/mist64/diorama64.git")
    if not linux:
        problem, note = common.xcode_problem()
        if problem:
            raise Stop(problem)
        if note:
            w(note)
        brew_step(asker)
    ensure_submodule("games/sm64/upstream", w)
    saved = load_config()
    want = us_sha1()
    if not want:
        raise Stop("cannot read the US ROM's SHA-1 from the pinned upstream (games/sm64/upstream)")
    if a.rom or not rom_ok(ROM, want) or (a.reconfigure and asker.interactive and
                                           not asker.yes(f"Keep the ROM in {ROM.relative_to(ROOT)}?", default=True)):
        src = clean_path(a.rom or asker.text("Path to your Super Mario 64 (US) ROM (drag it here)", "--rom PATH"))
        import_rom(src, ROM, want)
        w(f"ROM -> {ROM.relative_to(ROOT)} (Super Mario 64 US)")
    c = resolve(a, saved, asker)
    save_config(c)
    if linux:
        return quest_main(a, c, w)
    if c["target"] == "simulator" and not a.no_launch:
        import sim_util
        try:
            sim_util.device()
        except RuntimeError as e:
            raise Stop(str(e)) from None
    for s in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        step(s, write=w)
    names = prepare_packs(c.get("packs") or {}, w)
    data = ["--packs", ",".join(names)]
    if c["target"] == "headset":
        step("visionos_build.py", "--device", "--team", c["team"], "--bundle-id", c["bundle_id"], "--config", "Release", *data,
             write=w)
        if a.no_install:
            w("Built (not installed: --no-install).")
            return 0
        install(c["device"], c["bundle_id"], w, again=a.reinstall, label=c.get("device_name"))
        w(f"Diorama64 is on {c.get('device_name', c['device'])}. Start it on the headset.")
        return 0
    step("visionos_build.py", *data, write=w)
    if a.no_launch:
        w("Built (not launched: --no-launch).")
        return 0
    import visionos_run
    w("Launching in the simulator ...")
    res = visionos_run.run_scenario(SIM_OUT, ["--autostart", "--game", "sm64"], wait="AR_FIRST_FRAME", keep_running=True,
                                    timeout=300)
    if asker.interactive:
        subprocess.run(["open", "-a", "Simulator"], env=common.env())
    w(f"Running in the simulator ({res.get('wait', '').strip()}).")
    return 0


def run_main(argv):
    try:
        return main(argv)
    except Stop as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130
