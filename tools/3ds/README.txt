Banjo-Kazooie for Nintendo 3DS (Lighthouse 3DS port)
=====================================================

This is a homebrew app for the New Nintendo 3DS, New 3DS XL and New 2DS XL. It
contains no game data: it is made once from your own copy of the game. The
original 3DS, 3DS XL and 2DS are untested and will be much slower.

You need:
  * A 3DS with custom firmware (Luma3DS) and the Homebrew Launcher.
    See https://3ds.hacks.guide/ if you do not have these yet.
  * A Banjo-Kazooie ROM: US v1.0, US v1.1, PAL or Japanese, as .z64, .v64 or
    .n64.


1. Copy to the SD card
----------------------

Extract this zip onto the root of the SD card, then put your ROM in the
lighthouse folder (any file name; it can also go next to Lighthouse.3dsx or at
the root of the SD card). The card should look like this:

    3ds/
      Lighthouse.3dsx
      lighthouse/
        baserom.z64       <- your ROM
        extract/          <- from this zip
        lighthouse.o2r    <- from this zip

To copy without taking the SD card out, run ftpd on the 3DS and use any FTP
client.

To start it from the HOME Menu instead of the Homebrew Launcher, also install
Lighthouse.cia (in this zip) with FBI. It uses the same files in
3ds/lighthouse/, including your saves. On an Old 3DS it also gets more memory.


2. First start
--------------

Start Banjo-Kazooie from the Homebrew Launcher. The first time, the app makes
bk.o2r (the game's graphics, sound and text) from your ROM. On a New 3DS this
takes about a minute and a half, with a progress bar on the bottom screen.
Later starts use that bk.o2r, so the game no longer needs the ROM.

A bk.o2r made by a PC build of Lighthouse 3DS, built from source, works too:
put it in the lighthouse folder instead of the ROM. bk.o2r files made by other
Lighthouse versions, including official HarbourMasters releases, do not work
with this 3DS app. (A bk.o2r made for 3DS v1.0 or v1.1 works with either.)


3. Play
-------

The bottom screen shows a loading bar, then the current level's stats, your game totals and the frame
rate. On a New 3DS the game runs at 60 frames per second, also in 3D.

    3DS                     N64
    Circle Pad              Control stick
    A / B                   A / B
    L or ZL                 Z
    R or ZR                 R
    C-stick or D-pad        C buttons
    X                       C-up (first-person view)
    Y                       C-down
    START                   START
    START + SELECT          Quit to the Homebrew Launcher
    3D slider               Stereoscopic 3D (all the way down for 2D)

Saves, settings and a log file (logs/Lighthouse.log) are kept in
sdmc:/3ds/lighthouse/. If the game does not start, the log says why.

Updating: replace Lighthouse.3dsx, lighthouse.o2r and the extract folder with
the new release's. Keep your saves. If the new release says bk.o2r must be
remade, delete it and put your ROM back: the next start makes a new one.

Project page, source code and full documentation:
https://github.com/aidenator/Lighthouse-3DS
