# Diorama64

Diorama64 plays **Super Mario 64** as an augmented-reality diorama on **Apple Vision Pro** and **Meta Quest 3S**. The
level stands on your real table as a small model that you walk around, while you play with a game controller. It is
built on [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) with a custom Metal renderer (Vulkan on the Quest).

More info and videos: <https://www.pagetable.com/303>

Status: alpha.

## Disclaimer

Diorama64 is an unofficial fan project, not affiliated with or endorsed by Nintendo. It contains no game data; you need
your own Super Mario 64 (US) ROM.

## Requirements

- A Mac with Apple silicon.
- Xcode 26 or newer with the visionOS SDK (tested with Xcode 26.3).
- [Homebrew](https://brew.sh).
- Apple Vision Pro on visionOS 26 or later.
- An Apple Account.
- A Bluetooth game controller.

## Setup

```
git clone https://github.com/mist64/diorama64.git
cd diorama64
./build.py
```

`./build.py` checks your Mac, then asks for your ROM (any byte order), the HD texture and 3D model packs to add (Enter for
all, or `--packs`), your team and the headset. It builds Diorama64 and installs it on the headset. Your
ROM and the packs go inside the app. Running it again reuses the answers (`--reconfigure` asks again; `--help` lists
the flags).

On the headset, press **Open table** and look at your table. **Re-place table** in the window starts the search again.

## Settings

The app's window has four tabs (Level, World, Look, Audio). Every setting applies live and is saved.

## Meta Quest 3S

Requirements:

- A Linux PC.
- Meta Quest 3S in developer mode, with USB debugging allowed.

The toolchain comes from the [Nix](https://nixos.org) dev shell. Without Nix, install what `flake.nix` lists and set the
variables it exports (untested).

```
nix develop     # or: direnv allow
./build.py
```

`./build.py` asks for your ROM and the packs, builds Diorama64 and installs it on the connected Quest (`--no-install`
builds only).

The level stands on a table from the Quest's room scan; if there is none, the panel's **Scan the room (Space Setup)**
starts one. **Re-place table** moves it to the table you stand next to. The Quest starts in the Original N64 look:
Room-lit (Look tab) is too slow there for now with the Render96 models.

| Control | Action |
|---|---|
| Left stick | Move Mario |
| A, B or X | Jump, punch |
| Left trigger | Crouch (Z) |
| Y | Start |
| Grip and move | Drag the level |
| Grip and right stick | Turn the level |
| Left menu button | Settings panel (the same four tabs) |
| Right trigger | Point at menus |

The log: `adb logcat -s Diorama64`.

### Without a headset (Monado)

The same app runs on the Linux desktop against [Monado](https://monado.freedesktop.org)'s simulated headset, on a fixed
table and without passthrough. After one `./build.py`:

```
python3 scripts/desktop_xr.py
```

Click into Monado's debug window ("Monado! ✨⚡🔥") to drive it: WASD and right-mouse-drag move the head; left mouse is
the trigger, M is A, N is B, V is Start and TFGH the stick; hold Ctrl for the left hand (Ctrl+TFGH moves Mario, Ctrl+N
opens the panel). Ctrl-C in the terminal quits; `--help` lists the rest.

## Author

Claude, directed by Michael Steil &lt;mist64@mac.com&gt;.

## License

GNU Affero General Public License v3.0 only (AGPL-3.0-only); see LICENSE. The submodule keeps its own terms.
