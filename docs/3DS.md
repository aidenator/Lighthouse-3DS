# Nintendo 3DS

Lighthouse runs on the New Nintendo 3DS, New 3DS XL and New 2DS XL as a homebrew app. The original
3DS, 3DS XL and 2DS start and play in an emulator set to that model, but have not been tried on
real hardware, where they will be much slower.

The app contains no game data. The game's asset archive, `bk.o2r`, is made once from your own ROM
(US v1.0, US v1.1, PAL or Japanese), by the app itself on its first start. The game plays in the language of that ROM; the 3DS build has
no menu for choosing a language pack.

## Installing a release

You need a 3DS with custom firmware ([Luma3DS](https://3ds.hacks.guide/)) and the Homebrew Launcher,
and a Banjo-Kazooie ROM (see [Quick Start](../README.md#quick-start) for the supported ones).

1. From the [releases page](https://github.com/aidenator/Lighthouse-3DS/releases), download
   `Lighthouse-3DS-v<version>.zip`.
2. Extract it onto the root of the SD card, and put your ROM (`.z64`, `.v64` or `.n64`, any name)
   in `3ds/lighthouse/`, next to `lighthouse.o2r` and the `extract` folder. The app also looks next
   to `Lighthouse.3dsx` and at the root of the SD card, in that order, and takes the first supported
   ROM it finds (a file named `baserom` first in each folder); other games there are skipped.
3. Start **Banjo-Kazooie** from the Homebrew Launcher. On the first start the app makes `bk.o2r`
   from the ROM, in about a minute and a half on a New 3DS, with a progress bar on the bottom
   screen. Later starts use that `bk.o2r`. Leave the ROM on the card so that a release whose
   `bk.o2r` differs can make the new one by itself; without it, the app asks for the ROM then.
4. Optionally, install `Lighthouse.cia` from the zip with FBI, to start the game from the HOME Menu.
   It uses the same files and saves as the `.3dsx`. On an Old 3DS it runs in the 80 MB memory mode
   (`tools/3ds/cia/Lighthouse.rsf`).

An Old 3DS can't make `bk.o2r` itself: it needs about 56 MB of heap free, and an Old 3DS has 32 to
48 MB. So releases include PC builds for now: run the one from the same release, choose your ROM,
and put the `bk.o2r` it writes in `3ds/lighthouse/` instead of the ROM (`README-3DS.txt` says where
each one writes it).

`bk.o2r` files made by any other Lighthouse version, including official HarbourMasters releases, do
not work with the 3DS app. The app checks that `bk.o2r` matches it, and
remakes it from the ROM when a release changes what goes into it (v1.2 does, after v1.0 and
v1.1). The one the 3DS makes is stored uncompressed, so it is about
47 MB instead of 26 MB: compressing would add almost a minute on the 3DS's CPU.

## Building from source

This takes three steps:

1. Build the PC version from this branch and use it to turn your ROM into `bk.o2r`.
2. Build the 3DS app in Docker.
3. Copy the app and the two `.o2r` files to the SD card.

### What you need

* A New 3DS with custom firmware and the Homebrew Launcher
* A supported Banjo-Kazooie ROM in `.z64` format
* A Linux, macOS or Windows (WSL2) computer with Docker, CMake, Ninja, Python 3 and a C++ compiler

### 1. Generate the asset archives on the PC

The 3DS app reads two archives:

* `lighthouse.o2r`, the port's own assets, built from this repository
* `bk.o2r`, the game's assets, extracted from your ROM

A PC build of the same branch makes `bk.o2r` (or skip this and let the 3DS app make it from a ROM
on its first start; it then needs `config.yml` and `assets/` from this repository in
`3ds/lighthouse/extract/`). Use the PC build from this branch, not an official Lighthouse release:
the asset formats change between versions, and the 3DS app only accepts archives from its own
version.

```bash
git clone --recursive https://github.com/aidenator/Lighthouse-3DS.git
cd Lighthouse-3DS

cmake -H. -Bbuild-cmake -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --target GeneratePortO2R
cmake --build build-cmake
```

See [BUILDING.md](BUILDING.md) for the packages each platform needs.

Then run `build-cmake/Lighthouse` and choose your ROM when it asks. It writes `bk.o2r` to its data
folder: `~/.local/share/lighthouse/` on Linux, or the folder with the executable on Windows and
macOS. Once the game reaches its title screen you can quit.

Use the `bk.o2r` the game generates. The one made by the `ExtractAssets` build target has no
version stamp, and the 3DS app rejects it.

### 2. Build the 3DS app

The 3DS toolchain (devkitARM, libctru, citro3d) and the libraries Lighthouse needs are set up in a
Docker image. Build the image once:

```bash
docker build -t lighthouse-3ds -f tools/docker/3ds.Dockerfile tools/docker
```

Then configure and build from the repository folder:

```bash
docker run --rm -v "$PWD":/src -w /src lighthouse-3ds \
    cmake -S . -B build-3ds -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake
docker run --rm -v "$PWD":/src -w /src lighthouse-3ds cmake --build build-3ds
```

The app is `build-3ds/Lighthouse.3dsx`.

### 3. Copy to the SD card

```
sdmc:/
└── 3ds/
    ├── Lighthouse.3dsx
    └── lighthouse/
        ├── bk.o2r
        └── lighthouse.o2r
```

`lighthouse.o2r` is in the repository folder after step 1, and `bk.o2r` is in the PC build's data folder.

To copy without removing the SD card, run [ftpd](https://github.com/mtheall/ftpd) on the 3DS and
use any FTP client.

Start **Banjo-Kazooie** from the Homebrew Launcher. Startup takes a few seconds; the bottom screen
shows the startup log, then the version and frame rate.

## Controls

| 3DS | N64 |
| --- | --- |
| Circle Pad | Control stick |
| A / B | A / B |
| L or ZL | Z |
| R or ZR | R |
| C-stick (New 3DS) or D-pad | C buttons |
| X | C-up (first-person view) |
| Y | C-down |
| START | START |
| START + SELECT | Quit to the Homebrew Launcher |
| 3D slider | Stereoscopic 3D (all the way down for 2D) |

## Files on the SD card

`sdmc:/3ds/lighthouse/` also holds the saves (`saves/`), settings (`lighthouse.cfg.json`) and a log
(`logs/Lighthouse.log`). The log is the first place to look if the game does not start.

## Known issues

* A few particle effects that read back the screen are missing.
* The grayscale and noise screen effects are not drawn, and the bottom of the intro waterfall has a
  hard edge.
* Entering a level or restarting one can pause for about a second while assets load.

## Development

Builds can be sent straight to the 3DS without the SD card: in the Homebrew Launcher press Y to
start NetLoader, then run

```bash
docker run --rm --network host -v "$PWD":/src -w /src lighthouse-3ds \
    3dslink -a <3ds-ip> build-3ds/Lighthouse.3dsx -s
```

With `-s` the game's output and log stream back to the terminal (they arrive when the game exits).
The [Azahar](https://azahar-emu.org/) emulator also runs the app, with the `.o2r` files in its
`sdmc/3ds/lighthouse/` folder.

What made 60 fps and 3D possible, how much each change gained, and how to measure is in
[3DS-PERFORMANCE.md](3DS-PERFORMANCE.md). To tune the 3D while testing, hold SELECT and tap the
bottom screen: the top half moves the convergence distance (left nearer, right farther), the bottom
half the HUD's depth (left out, right in). The values show on the bottom screen and in the log.

### Making a release

Pushing a tag named `3ds-v` plus the version in `LH_3DS_VERSION` (`CMakeLists.txt`), such as
`3ds-v1.0`, runs `.github/workflows/release-3ds.yml`. It builds `lighthouse.o2r`, the 3DS app and
the PC builds from that commit (set `skip-pc-builds` in the workflow to leave the PC builds out, once
an Old 3DS can make `bk.o2r` itself) and creates a draft GitHub release with install notes. Its "Changes" section comes
from `docs/release-notes/<tag>.md`, such as `docs/release-notes/3ds-v1.2.md`, if that file exists;
write it before tagging, including whether `bk.o2r` has to be made again. The release stays a
draft until it is published by hand. The tag must match `LH_3DS_VERSION`, or the release job fails.

```bash
git tag 3ds-v1.2
git push origin 3ds-v1.2
```

The zip's `README-3DS.txt` is `tools/3ds/README.txt`.
