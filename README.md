# KisakCOD VR for WinlatorXR

[![Sponsor](https://img.shields.io/badge/Sponsor-SgtBilko76-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/SgtBilko76)

Call of Duty 4: Modern Warfare (2007) in VR, standalone on a Meta Quest: no
PC, no streaming, no OpenXR or SteamVR runtime. The game runs inside a
[WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) container and talks to
the headset through WinlatorXR's
[XrAPI](https://winlatorxr.github.io/xrapi.html).

This is a fork of [KisakCOD VR](https://github.com/jplakon/CallOfDuty4_VR),
the single-player OpenXR VR conversion of COD4 by jplakon, which is itself
based on [KisakCOD](https://github.com/SwagSoftware/KisakCOD). It adds a
WinlatorXR backend to it. For PC VR (SteamVR, Virtual Desktop, Quest Link),
use the upstream project.

It contains no Call of Duty game data. You need your own installed copy of
the original 2007 Windows release.

- [Download the current beta](https://github.com/SgtBilko76/CallOfDuty4_VR/releases)
- [Full WinlatorXR guide](docs/WINLATORXR.md): setup, controls, settings,
  how it works

## Current status

The current beta is
[`v0.2.0-beta.1`](https://github.com/SgtBilko76/CallOfDuty4_VR/releases/tag/v0.2.0-beta.1).

Tested on a Meta Quest 3 with WinlatorXR Dawn (`dawn-33`) at `2388x1080`
(1194x1080 per eye): Crew Expendable runs at about 26 to 50 frames per
second, menus and videos at 72. The campaign has been played from the intro
through F.N.G. into Crew Expendable.

**What works**

- Stereo rendering with head and controller tracking, paced to the headset's
  own frame rate
- Physical weapon handling: aiming, two-hand grip, magazine reloads and belt
  grenades
- Physical scope (experimental): scoped weapons get their own magnified view,
  drawn as a round lens on the optic
- Menus and cinematics on a screen fixed in the room, navigated like a
  gamepad
- WinlatorXR Dawn and the older cats-27 builds

**New in beta 0.2**

- Gameplay no longer drops to one frame per second (a compiler issue in the
  Linux build set the game's speed to zero)
- Stereo on public Winlator builds, which open the game window with a title
  bar: the game now pins its window borderless so WinlatorXR finds it
- Gamepad-style menus: left stick, A and B, with no pointer
- The physical scope lens
- A Dawn package with a ready-to-import container profile

**Known limitations**

- The physical scope still needs confirming on every scoped weapon
- No manual recenter for the menu screen
- Quest 2 and Pico are untested
- WinlatorXR's right-stick-click menu can switch back to a flat window; click
  again to return to VR

See [docs/WINLATORXR.md](docs/WINLATORXR.md#known-limitations) and
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) for the full lists.

## Requirements

- A Meta Quest 3 (other headsets untested)
- WinlatorXR: Dawn (`dawn-33` or newer) recommended, cats-27 also works
- Call of Duty 4: Modern Warfare (2007, Windows), your own copy
- A USB cable and `adb`, or a file manager on the headset, to copy files

## Install

Each release has two downloads:

| Your WinlatorXR | Download | Follow |
| --- | --- | --- |
| Dawn (`dawn-33` or newer) | `KisakCOD-VR-WinlatorXR-Dawn-<version>.zip` | `README_DAWN.txt` |
| cats-27 and others | `KisakCOD-VR-WinlatorXR-<version>.zip` | `README-FIRST.txt` |

In short:

1. Copy your COD4 folder (the one with `iw3sp.exe`) to the headset as
   `Download/CallOfDuty4`, which is `D:\CallOfDuty4` inside the container.
2. Extract the release into that folder, overwriting `binkw32.dll`,
   `mss32.dll` and the `miles` folder.
3. Set up the container: screen size `2388x1080`, DXVK, the Turnip wrapper
   driver, XR API on, and drive `D:` mapped to `/sdcard/Download`. On Dawn,
   import the included `.wxrprofile.json` instead of setting these by hand.
4. Create a shortcut that runs
   `wine C:\windows\system32\cmd.exe /c D:\CallOfDuty4\Launch-KisakCOD-VR-WinlatorXR.bat`,
   or use the included `CallOfDuty4-VR.desktop`.
5. Put the headset on and start it. The first launch compiles shaders and
   takes a while.

Updating from beta 0.1: replacing `KisakCOD-sp.exe` is enough.

## Controls

| Action | Button |
| --- | --- |
| Fire | Right trigger |
| Use / pick up | X |
| Reload / eject magazine | A |
| Crouch / stance | B |
| Next weapon | Y |
| Pause | Menu |
| Support grip, magazines, grenades | Left grip |
| Grenade launcher | Right grip |
| Sprint | Left stick click |

In menus the left stick moves the selection, A confirms and B goes back. The
right stick click is left free for WinlatorXR's own menu. Reloading and
grenades are physical: see [docs/WINLATORXR.md](docs/WINLATORXR.md#controls).

## Build from source

The releases are cross-built from Linux with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw). Put an `msvcrt` build
of it under `~/toolchains` (or set `KISAK_MINGW_ROOT`), then:

```sh
cmake -B build-mingw-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=scripts/toolchains/windows-x86-mingw.cmake
cmake --build build-mingw-rel --target KisakCOD-sp
```

The executable and its DLLs land in `bin/`. `tools/make_dawn_package.sh
<version>` builds the Dawn release zip from the committed tree.

The toolchain file passes `-mlong-double-64`, and it has to: the decompiled
engine reads floats back through `long double`, which only works where that
type is 8 bytes, as on MSVC. Without it, gameplay runs at one frame per
second.

The upstream MSVC build (`scripts\mksln.bat`, Visual Studio 2022) still works
for Windows; see the
[upstream README](https://github.com/jplakon/CallOfDuty4_VR#build-from-source).

## Reporting bugs

Open an issue in this repository with your headset, WinlatorXR build, the
container settings, and `main\console.log` from the game folder. Lines
starting with `[VR][WINLATORXR]`, `[WATCHDOG]` and `[FRAMEPHASE]` are the
useful ones.

## Credits

- [jplakon](https://github.com/jplakon/CallOfDuty4_VR): KisakCOD VR, the VR
  conversion this fork builds on
- [SwagSoftware](https://github.com/SwagSoftware/KisakCOD): KisakCOD
- [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR): the container and
  XrAPI
- Call of Duty 4: Modern Warfare is (C) Infinity Ward / Activision

Licensed under the GPLv3; see [LICENSE](LICENSE) and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
