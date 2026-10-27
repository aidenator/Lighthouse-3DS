# Roadmap

What's planned for the 3DS port after v1.0. Plans change; this is a list of intentions, not
promises.

## v1.2

* **`bk.o2r` made on the 3DS.** Put the ROM in `sdmc:/3ds/lighthouse/` and the app makes `bk.o2r`
  on the first start, in about 80 seconds on a New 3DS, with a progress bar; a PC is no longer
  needed. This builds Torch, Lighthouse's asset extractor, into the app. Torch used to hold the
  whole archive in memory (about 730 MB); it now writes each asset as it goes and peaks under
  50 MB, with the same output. Writes go to the SD card in whole aligned 1 MB blocks: unaligned
  writes ran at 0.1 MB/s. The app looks for the ROM in `3ds/lighthouse/`, next to the `.3dsx`
  and at the SD card's root, skips other games, and never keeps an archive with assets missing.
* **The 3DS zip is all a New 3DS needs.** The PC builds stay in the release for the Old 3DS, which
  can't make `bk.o2r` itself yet; the release workflow's `skip-pc-builds` drops them once it can.
* **3D on the file select** uses its own convergence (200), which suits that scene better.
* **`bk.o2r` updates itself.** The build stamps a hash of what makes `bk.o2r` (Torch's commit, the
  asset lists, `config.yml`) into the archives the app makes, and the app remakes an archive with
  another stamp from the ROM, replacing the old one only once the new one is complete. A version
  bump alone no longer asks for a new `bk.o2r`.
* **A `.cia`** to install to the HOME Menu, with a banner and icon, built alongside the `.3dsx`
  (`tools/3ds/cia/`). It asks for the 80 MB memory mode on an Old 3DS.
* **Lighthouse 1.2.0**: upstream's changes since this port branched, merged into this branch, its
  libultraship and its Torch.
* **Freezes fixed.** The lock on the task list the game and audio threads share never locked, so an
  audio task could overwrite the frame's graphics task, and the game waited for good. Found with a
  trace of the threads involved (`LH_HALT_ON_LOST_TOKEN`). Separately, the 3DS can miss a
  condition variable's wake-up, which once stopped the renderer's handoff after hours, so waits for
  a single reply check again in slices there (`port/OS/Wait.h`).
* **SDL's audio thread** starts on core 0 when the system won't give it core 1, instead of failing
  with "Couldn't create audio thread".

## v1.1 (released 2026-10-24)

* **60 frames per second on the New 3DS.** Frame interpolation is back on, and Freezeezy Peak, the
  heaviest level, holds about 60. The renderer records each frame and a second thread on core 2
  sends it to the GPU, and the per-draw and per-triangle costs in libultraship's citro3d backend
  were cut until a heavy frame takes about 9 ms of the 16.6 ms budget.
* **More cores in use.** On a New 3DS:

  | Core | Work |
  |---|---|
  | 0 | Renderer and main loop |
  | 1 | The game's audio thread (the app borrows 80% of this core's time) |
  | 2 | Game logic, and sending recorded frames to the GPU |
  | 3 | Building in-between frames, when the system grants the core |

* **A loading screen and a stats screen** on the bottom screen. While the game starts, a progress
  bar, the step under way and the latest log line, with errors kept up in red. During play, the
  current level's notes, Jiggies, honeycombs and time, the game's totals and the frame rate.
* **Faster startup**: about 2.5 seconds, down from about 5 (and sometimes over 10). Settings are no
  longer rewritten to the SD card when nothing changed, and SDL no longer looks for a romfs the app
  doesn't have.
* **Clear errors when `bk.o2r` is missing or broken.** If `lighthouse.o2r` or `bk.o2r` is missing,
  `bk.o2r` can't be read (damaged or only partly copied), or it was made by another Lighthouse
  version, the bottom screen says so and how to fix it, and waits for START before exiting. A
  damaged `bk.o2r` used to hang startup.
* **Stereoscopic 3D at 60 fps on the New 3DS**, with the 3D slider. Each eye's view shifts by
  depth, with Banjo about at the screen's depth. The HUD and pause menu have a depth of their own,
  and the jigsaw transition comes out of the screen as its pieces fly at the camera. With the
  slider down, the game draws one image, at no extra cost. Textures now live in VRAM, and the GPU
  starts on the left eye while the right one is built. Details in
  [3DS-PERFORMANCE.md](3DS-PERFORMANCE.md#stereoscopic-3d).
* **Old 3DS check.** On an Old 3DS (two cores), the game logic and the work of sending frames to
  the GPU fall back to core 0 beside the renderer. Tested in the Azahar emulator in Old 3DS mode
  (boots, and fits in an Old 3DS's memory) and on a New 3DS made to act like an Old one (268 MHz,
  no L2 cache, cores 0 and 1 only): no crashes or freezes, but far too slow to play. See
  [Old 3DS speed](#old-3ds-speed).

## Later

### Old 3DS speed

On a simulated Old 3DS the game runs at about 10 frames a second, and the game itself slows to
about 10 updates a second instead of 30, so everything moves in slow motion. The CPU work measured
per frame (about 46 ms) would allow about 20 frames a second, but every slow frame takes exactly
98 ms, which points at a wait that runs into a timeout rather than a CPU that is too slow: the game
logic and the renderer seem to wait on each other each frame. Places to look: the threads that now
share core 0 at the same priority (a 3DS thread does not give way to another of equal priority
until it blocks), and the audio thread's share of core 1, where the system's graphics service
also runs.

Making `bk.o2r` needs about 56 MB of heap free, and an Old 3DS has 32 MB (the `.cia`, 80 MB mode)
to 48 MB. The plan: when `bk.o2r` has to be made, the app restarts itself into a mode that gives
nearly all its memory to the heap (graphics memory is fixed when a program starts), makes
`bk.o2r`, and restarts into the game.

The 98 ms waits may be worth another look now that the 3DS's missed condition-variable wake-ups
are known (v1.2): the waits that used to sleep until woken now check again in slices.

To test without an Old 3DS, configure with `-DLH_3DS_SIMULATE_OLD=ON` (and
`-DLH_3DS_OLD_INTERPOLATION=ON` to also draw in-between frames). The log's `game:` line shows the
game's own update rate.

### A better bottom screen

The bottom screen is a text console with the loading bar, the current level's stats, the game's
totals and the frame rate. Make it more useful during play, for example:

* **More stats**: Mumbo tokens, empty honeycombs and Jinjos per world, like the pause menu's totals.
* **An overhead map** of the current world with Banjo's position. The game has no maps of its own,
  so they would have to be drawn from each level's geometry, either ahead of time into images in
  `bk.o2r` or live as a cheap top-down render.
* A settings menu, for options such as draw distance and controls, and the 3D's convergence and
  HUD depth, which today are fixed (with a hidden SELECT + tap control for testing).
* **The game's language.** A PAL ROM has English (UK), French and German, and Lighthouse can
  switch between them, but on the 3DS only by editing `DialogLanguage` in `lighthouse.cfg.json`
  (English (UK) `1583480766`, French `2092788901`, German `286263347`). It belongs in the settings
  menu; until then, a quick way would be SELECT + L/R on the file select, with the language's name
  on the bottom screen.

Drawing the bottom screen with the GPU instead of the text console costs some renderer time, so it
has to fit beside 60 fps.

### Lighthouse features

Bring back the PC version's features where they fit on the 3DS, for example:

* **The randomizer.**
* **Mods**, in three kinds:
  * Asset mods: `.o2r` files that replace models, textures or sounds, dropped in
    `3ds/lighthouse/mods/`. Slim builds don't load the `mods` folder yet.
  * Romhacks: made from a hacked ROM, which the app could extract on the 3DS the way it does the
    base game. Torch already supports them on PC, and the `extract` folder already has the
    `hashes.yaml` it needs to skip the assets a hack leaves unchanged.
  * Code mods: the 3DS can't load code at runtime, so these would have to be built into the app.

  Mod authors make and test mods with PC tools: a PC build from source, or a PC download in the
  release again once mods are supported.

### Known issues

The [known issues](../README.md#known-issues) in the README: the particle effects that read back
the screen, the grayscale and noise effects, the intro waterfall's edge, and the pause when a level
loads.
