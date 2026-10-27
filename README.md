# Lighthouse 3DS

Banjo-Kazooie on the New Nintendo 3DS, as a homebrew app. This is a port of
[Lighthouse](https://github.com/HarbourMasters/Lighthouse), HarbourMasters' native PC port of the
game, to the 3DS's own graphics hardware.

<p align="center">
  <img alt="Banjo and Tooty playing in the opening cutscene on the top screen; the bottom screen shows Banjo-Kazooie, v1.0 and 20 fps" src="docs/images/3ds-intro.png" width="400">
  &nbsp;
  <img alt="The Banjo-Kazooie title screen on the top screen, with the version and frame rate on the bottom screen" src="docs/images/3ds-title.png" width="400">
</p>

* 60 fps on a New 3DS, also in stereoscopic 3D, and starts in about three seconds
* Saves to the SD card
* All four versions of the game: US v1.0, US v1.1, PAL and Japanese
* Controls laid out for the 3DS, with the C buttons on both the C-stick and the D-pad

No game data is included. The app makes it once from your own copy of the game, on its first start.

## Download and install

Get the latest version from [Releases](https://github.com/aidenator/Lighthouse-3DS/releases). You
need a 3DS with custom firmware ([Luma3DS](https://3ds.hacks.guide/)) and the Homebrew Launcher, and
a Banjo-Kazooie ROM.

1. **Copy everything to the SD card.** Extract `Lighthouse-3DS-v<version>.zip` onto the root of the
   SD card and put your ROM (`.z64`, `.v64` or `.n64`) in `3ds/lighthouse/`, next to
   `Lighthouse.3dsx` or at the root of the card:
   ```
   3ds/
   ├── Lighthouse.3dsx
   └── lighthouse/
       ├── baserom.z64     <- your ROM, any name
       ├── extract/
       └── lighthouse.o2r
   ```
2. **Play.** Start **Banjo-Kazooie** from the Homebrew Launcher. On the first start the app makes
   `bk.o2r`, the game's graphics, sound and text, from your ROM: about a minute and a half, with a
   progress bar on the bottom screen. Later starts use that `bk.o2r`; leave the ROM on the card so a
   release that needs a new one can make it by itself. START + SELECT quits.
3. **Optional: install to the HOME Menu.** Install `Lighthouse.cia`, also in the zip, with FBI. It
   starts from the HOME Menu and uses the same files and saves.

An Old 3DS can't make `bk.o2r` itself (not enough memory). Make it on a computer with the PC build
from the same release, and put it in `3ds/lighthouse/` instead of the ROM. A `bk.o2r` made by any
other Lighthouse version will not work.

`README-3DS.txt` in the zip has the full steps. Your ROM must be one of these
(check with any SHA-1 tool):

| Version | SHA-1 |
| --- | --- |
| US v1.0 | `1fe1632098865f639e22c11b9a81ee8f29c75d7a` |
| US v1.1 | `ded6ee166e740ad1bc810fd678a84b48e245ab80` |
| PAL | `bb359a75941df74bf7290212c89fbc6e2c5601fe` |
| Japanese | `90726d7e7cd5bf6cdfd38f45c9acbf4d45bd9fd8` |

The game plays in the language of the ROM you use. A `.n64` or `.v64` ROM must be converted to
`.z64` first.

## Controls

| 3DS | N64 |
| --- | --- |
| Circle Pad | Control stick |
| A / B | A / B |
| L or ZL | Z |
| R or ZR | R |
| C-stick or D-pad | C buttons |
| X | C-up (first-person view) |
| Y | C-down |
| START | START |
| START + SELECT | Quit to the Homebrew Launcher |
| 3D slider | Stereoscopic 3D (all the way down for 2D) |

## Supported systems

* **New Nintendo 3DS, New 3DS XL, New 2DS XL:** supported, and what this port is tested on.
* **Original 3DS, 3DS XL, 2DS:** starts and plays in an emulator, but untested on real hardware,
  where it will be much slower.

Saves, settings and a log file are kept in `sdmc:/3ds/lighthouse/`. If the game does not start,
`logs/Lighthouse.log` there says why.

## Known issues

* A few particle effects that read back the screen are missing.
* The grayscale and noise screen effects are not drawn, and the bottom of the intro waterfall has a
  hard edge.
* Entering or restarting a level can pause for about a second while it loads.

What's planned next is in [docs/ROADMAP.md](docs/ROADMAP.md).

## Building from source

See [docs/3DS.md](docs/3DS.md): it covers making the asset archives, building the 3DS app with the
included Docker toolchain, sending builds to a 3DS over Wi-Fi, and making a release.

The PC version still builds from this repository too; [docs/LIGHTHOUSE.md](docs/LIGHTHOUSE.md) is
the original Lighthouse README.

## Credits

This port stands on the work of others:

* [Lighthouse](https://github.com/HarbourMasters/Lighthouse) by HarbourMasters: the PC port this is
  built on (developers Malkierian, JeodC, Caladius and aMannus)
* [libultraship](https://github.com/Kenix3/libultraship): the engine layer; the 3DS renderer and
  platform code live in [this port's fork](https://github.com/aidenator/libultraship/tree/3ds)
* The [Banjo-Kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie) team
* [devkitPro](https://devkitpro.org/), libctru and citro3d: the 3DS toolchain and libraries

3DS port by aidenator.

This project is not affiliated with or endorsed by Nintendo, Rare or HarbourMasters. It contains
no game assets; you must provide your own legally obtained copy of the game. Please do not ask for
or share ROMs.

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
