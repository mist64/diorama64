# Diorama64

Diorama64 plays **Super Mario 64** as an augmented-reality diorama on **Apple Vision Pro**. The level stands on your real
table as a small model that you walk around, while you play with a game controller. It is built on
[sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) with a custom Metal renderer.

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

Requirements: Linux with [Nix](https://nixos.org) and [direnv](https://direnv.net), a Quest 3S in developer mode with
USB debugging allowed, and your own Super Mario 64 (US) ROM.

```
direnv allow     # or: nix develop
./build.py
```

`./build.py` asks for your ROM and the packs, builds the app and installs it on the connected Quest (`--no-install` builds
only). The Vision Pro instructions above are unchanged.

| Control | Action |
|---|---|
| Left stick | Move Mario |
| A | Jump |
| B or X | Punch |
| Left trigger | Crouch (Z) |
| Y | Start / pause |
| Grips | Drag the level on the table |
| Right stick, while gripping | Turn the level |
| Left menu button | Open the settings panel |
| Right trigger + aim | Point at menus (masked in a level) |

The level sits on a scanned table. If Space Setup has no table yet, the panel offers a scan.

Settings are saved as `prefs.json`, `placement.json` and `pipelines.txt` in the app's files dir. The log:
`adb logcat -s Diorama64`. Debug: `adb shell am start -n org.pagetable.diorama64/android.app.NativeActivity --ez fixed_table true` skips the table.

## Author

Claude, directed by Michael Steil &lt;mist64@mac.com&gt;.

## License

GNU Affero General Public License v3.0 only (AGPL-3.0-only); see LICENSE. The submodule keeps its own terms.
