### Upgrading from v1.1

1. Extract this zip onto the root of the SD card, over the old files. This replaces
   `3ds/Lighthouse.3dsx` and `3ds/lighthouse/lighthouse.o2r`, and adds the `extract` folder.
2. **Put your Banjo-Kazooie ROM on the SD card**, in `3ds/lighthouse/`. This release is based on
   Lighthouse 1.2.0, whose `bk.o2r` is different, so the old one has to be made again. The app
   does that by itself on the first start, from the ROM, in about a minute and a half, and replaces
   the old `bk.o2r` only once the new one is complete.

Your saves and settings carry over.

### New

- **No PC needed.** The app makes `bk.o2r` from your ROM (`.z64`, `.v64` or `.n64`, any name) on
  the first start, with a progress bar, in about 75 seconds on a New 3DS. Put the ROM in
  `3ds/lighthouse/`, next to `Lighthouse.3dsx` or at the root of the SD card; other games there are
  skipped. Leave the ROM there to have future releases update `bk.o2r` on their own; if you
  delete it, a release that needs a new `bk.o2r` asks for it again.
- **`bk.o2r` updates itself.** When a release changes what goes into `bk.o2r`, the first start
  remakes it from the ROM ("Updating bk.o2r for this version"). A release that doesn't change it
  keeps the one you have, version bump or not.
- **Install to the HOME Menu.** `Lighthouse.cia` in this zip installs with FBI and starts from the
  HOME Menu, with its own banner and icon. It uses the same files and saves as the `.3dsx`.
- **Lighthouse 1.2.0.** The PC port's latest changes are merged in, among them fixes like
  Clanker's tooth no longer spawning Jiggies endlessly. Most of its new options need a settings
  menu, which the 3DS build doesn't have yet.
- **3D on the file select** is tuned for that scene.

### Fixes

- **A rare freeze on a black or still screen**, for example entering a level, after anything from
  a few minutes to a few hours. The game's audio and the frame being drawn could pick the same
  slot in a shared task list, and the frame was lost. A second, rarer hang in the renderer's
  handoff between threads is fixed too. The game now ran 7.5 hours straight without either.
- **"SDL_OpenAudio error: Couldn't create audio thread"** on the loading screen, seen on some
  consoles: the audio thread now starts on another core when the system won't give it core 1.

### Known issues

- The Old 3DS, 3DS XL and 2DS run the game far too slowly to play (about 10 fps), and can't make
  `bk.o2r` themselves: there isn't enough memory. Make it on a New 3DS, or with this release's PC
  build (see the downloads above).
- A PAL ROM has English, French and German, but choosing one needs a settings menu that isn't
  there yet. Until then, set `DialogLanguage` in `3ds/lighthouse/lighthouse.cfg.json`: English (UK)
  `1583480766`, French `2092788901`, German `286263347`.
- The `bk.o2r` the 3DS makes is about 47 MB, against 26 MB from a PC: compressing it would add
  almost a minute on the 3DS.
- Unchanged from v1.0: a few particle effects that read back the screen are missing, the grayscale
  and noise screen effects are not drawn, and entering a level can pause for about a second.
