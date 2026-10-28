# 3DS performance notes

What got the 3DS port from 30 fps to 60 fps on a New 3DS, then to 60 fps in stereoscopic 3D, how
much each change gained, and what didn't work. The last section covers making `bk.o2r` on the 3DS
itself. All numbers were measured on a New 3DS, from the frame stats the app logs every 3
seconds (see [Measuring](#measuring)).

## How a frame is drawn

1. **Core 2, game logic.** The game builds N64 display lists at 30 updates a second (20 in many
   cutscenes).
2. **Core 0, interpreter.** libultraship's interpreter (`src/fast/interpreter.cpp`) runs the
   display lists on the CPU: it transforms and lights the vertices, works out the colour combiner
   and textures, and passes batches of finished triangles to the citro3d backend.
3. **Core 0, recording.** The backend (`src/fast/backends/gfx_citro3d.cpp`) doesn't call citro3d
   here. It records each call, with its vertices, into a frame record.
4. **Core 2, replay.** A replay thread turns the record into citro3d state, vertex data and draw
   calls, then ends the GPU frame.
5. **Core 3, interpolation.** With frame interpolation on, a helper builds the in-between frames,
   so the interpreter runs twice per game update.

At 60 fps every frame, real or in-between, has to fit in 16.6 ms.

## What mattered most

The test spot was the heaviest view found in Freezeezy Peak: about 210 draws and 1,400–1,500
triangles. These are in the order they were done:

| Change | fps at the spot | What it saved |
|---|---|---|
| Interpolation on, before the work below | about 32 | |
| Texture units: enable only those the combiner reads | 32 → 36 | GPU time |
| Mid-frame batching taken back out (see [What didn't work](#what-didnt-work)) | 36 → 39 | Its own cost |
| Textures in the smallest lossless format | 39 → 43 | 1.5 ms of GPU wait |
| **Deferred replay**: record the frame, replay it at the end | **43 → 52** | 4.4 ms of GPU wait, down to 0 |
| Replay thread on core 2 | 52 → 55–56 | Render thread: 17.7 → 14.5 ms |
| **Four small CPU cuts** (see below) | **54 → 59.5–60** | Display lists: 14.7 → 11 ms |
| Per-triangle trims | Worst window: 56.4 → 59.5 | Heavy frames: 9.9 → 8.8 ms |

The replay-thread build measured 55–56 fps, and 54 in the next run, which added more timers.

Ranked by gain at the spot:

1. **Overlapping the CPU and GPU (deferred replay), +9 fps.** Before, each frame started by waiting
   for the GPU to finish the last one, then interpreted the display lists, so frame time was CPU
   time plus GPU time. Recording the frame and replaying it at the end lets the interpreter run
   while the GPU draws the previous frame, so frame time became whichever of the two is slower.
2. **Small CPU costs that are expensive on a 3DS, +5 fps, 3.7 ms.** Four changes that would mean
   nothing on a PC:
   * **`weak_ptr` locks.** All 100 display-list command handlers locked a `weak_ptr` to the
     interpreter on every call: two atomic updates with memory barriers each time. They now use a
     plain pointer. This and the next change made up most of the 3.7 ms.
   * **Clock reads.** Reading the clock is a kernel call on the 3DS. Every texture lookup read it
     twice for a stats counter, and BK asks for the same texture again 40–60% of the time. Now
     only real texture decodes, past the cache, are timed.
   * **Copies in `SETTIMG`.** The texture metadata holds a `shared_ptr` and was copied three times
     per command. It is now moved.
   * **The vertex loop.** The matrix and the aspect-ratio scale are worked out once per vertex
     load, not for every vertex.
3. **Smaller textures, +4 fps.** Every texture was stored as 32-bit RGBA8. Now each is stored in the
   smallest format that holds it without loss (L8, LA4, LA8, RGBA5551, RGBA4), which is 8 or 16
   bits for about 87% of BK's textures. The GPU reads half as much per pixel or less.
4. **Enabling only the texture units the combiner reads, +4 fps.** Unused units bound to a
   texture still cost the GPU time.
5. **The replay on its own core, +3–4 fps.** Turning the record into GPU commands took about 35%
   of the render thread. On core 2, beside the game logic (which leaves most of that core idle at
   30 updates a second), frame N is replayed while frame N+1 is interpreted. This adds one frame of
   delay, which isn't noticeable.
6. **Per-triangle trims, about 1 ms in heavy frames.** The interpreter writes each batch's vertices
   straight into the frame record, which saves copying about 235 KB a frame. Back-face culling
   uses the clip-space determinant instead of three divisions per triangle. This fixed the last
   dips, which came when flying over the whole level.

### Earlier changes that 60 fps depended on

* **Not waiting for vblank before each frame.** `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` waited for the
  display's next refresh, which the game's own frame timer isn't in step with, so every frame lost
  up to 16.7 ms (about 9 on average) before drawing started. With `C3D_FrameBegin(0)`, it only
  waits for the GPU's last frame. At 30 fps this took heavy Freezeezy views from 22–27 fps to
  29–30. At 60, the swap waits for vblank only if none has passed since the last frame
  (`C3D_FrameCounter`).
* **Caching the per-triangle state.** The interpreter redid its setup for every triangle:
  texture sizes, the combiner, the shader lookup and several divisions. Now it does it only when
  the game changes state. Triangle setup fell from about 25% of drawing time to about 15%.
* **Audio on core 1.** The game's audio thread was on core 0 with the renderer. It now runs on
  core 1, which a New 3DS app can borrow up to 80% of (`APT_SetAppCpuTimeLimit`). The gain wasn't
  measured on its own.
* **`-O3 -ffast-math`**, as the Switch build uses. These exposed an undefined-behaviour bug in the
  game code: a three-entry hit-counter array declared as two bytes.

## What didn't work

* **Sending draws to the GPU in batches mid-frame.** No gain. citro3d keeps its GPU queue stopped
  until the frame ends, so batches sent early just wait. Starting the queue from outside citro3d
  hung the game a few seconds into the intro. Deferred replay got the overlap instead.
* **Skipping combiner, depth and blend state that matches the last draw.** Barely measurable in
  Freezeezy, where most draws change state. It was kept anyway, since it costs nothing.

## Bugs to avoid

* **A "replaying" flag shared by two threads.** With the replay on its own thread, the render
  thread saw the flag set and sent its next frame's draws straight to citro3d, mixed in with the
  replay's. That caused a black haze, wrong lighting, and a crash once the command buffer
  overflowed. The flag is `thread_local`.
* **`C3D_TexBind` with no texture.** It reads the texture's format before checking it, and crashes.
  Units that shouldn't sample get a small dummy texture.

## Measuring

* **Stats every 3 seconds**, streamed to the PC with `3dslink -s`. They show fps, draws,
  triangles, GPU time, draw time (average and worst), replay time, time spent waiting for the GPU
  and for the replay, time between frames and the vblank wait, and heap and linear memory.
  The output arrives when the app exits.
* **A sampling profiler** on core 1 reads the renderer's current phase (a display-list command or
  a backend step) every 200 µs. The stats show the top 10 for the renderer and for the replay.
  Unlike timers around each call it costs the renderer nothing, which matters when each clock read
  is a kernel call. Per-call timers slowed the frames they were measuring.
* **GPU tests: SELECT + R** cycles through a tiny scissor (tests the cost of filling pixels), no
  blending, and no draws. The current test shows on the bottom screen.
* **A slow-frame report**: any frame over 70 ms logs its draw time and what loaded during it.
* **The game's update rate**: the `game:` line shows game-thread busy time per frame and how long
  it waited.
* **Use the same spot every time.** The triangle count depends on where the camera points, since
  the game culls what's off screen and swaps in simpler models at a distance. Compare runs at a
  fixed view, not across a play session.

## Stereoscopic 3D

With the 3D slider up, the replay thread replays each recorded frame twice, once into each eye's
render target. The vertex shader shifts each vertex sideways by its depth:
`shift * max(w - convergence, -cap * w)`. The interpreter's clip-space `w` is the distance from the
camera, so this is one multiply-add per vertex, and the right eye reuses the left eye's vertices.
The interpreter (core 0) does no extra work.

At the Freezeezy spot (about 215 draws and 1,300 triangles), with the slider all the way up:

| Change | fps | What it saved |
|---|---|---|
| Both eyes, first version | 48–50 | GPU time doubled to 13–15 ms |
| **Textures in VRAM** | **57–58** | GPU time 13–15 → about 9.3 ms |
| Flush only what the CPU wrote, not all of linear memory | 57–58 | Not measurable here |
| **Start the GPU on the left eye while the right one is built** | **59.5–60** | The replay's wait for the GPU, 9.8 → 0.1–0.3 ms |

* **Textures in VRAM.** The GPU samples VRAM much faster than linear memory. Uploads still go
  through linear memory, then the copy engine moves them over (`GX_TextureCopy` with the plain
  contiguous form, as citro3d's own `C3D_TexLoadImage` uses). All of a frame's uploads are done
  before its draws, so each is one queue entry, at most 8 a frame. Textures past that go to linear
  memory and move over in later frames. About 4.6 MB of VRAM is left after the two eyes' targets.
* **Overlap.** citro3d keeps its GPU command queue stopped until the frame ends, so the replay
  built a whole frame, then the GPU drew it, and the two added up: about 7.5 + 10 ms. Calling
  `gxCmdQueueRun` on citro3d's queue (the first member of `__C3D_Context` in citro3d 1.7.1) after
  the left eye starts the GPU on that eye while the right one is built.
* **Depth rules.** The world uses a convergence of 384 (`w` units), and close-ups come out at most
  as far as the distance goes in. The HUD gets a fixed depth (−0.5, slightly out of the screen).
  That covers rectangles (`w` = 1) and anything drawn without depth testing after the world's last
  depth-tested draw, since the game draws its counters and pause menu in perspective. The jigsaw
  transition has a camera of its own and sets `G_EX_STEREO_OWN_CAMERA`, which measures its depth
  against its own convergence (150, measured). Draws into offscreen framebuffers, and copies of the
  screen drawn back, aren't shifted.
* **Tuning.** The bottom screen's menu sets the convergence (3D focus) and the HUD's depth while
  the game runs, so the effect shows at once.

### Bugs to avoid in 3D

* **citro3d's GPU command queue holds 32 entries.** A VRAM upload that sent the draws before it,
  then queued its copy, took two. A frame that loaded dozens of textures (skipping a cutscene)
  overflowed it, and `gxCmdQueueAdd` panicked.
* **Never stop a GX queue while it runs.** libctru stops counting completions once a stop is
  requested, so the queue never looks finished and the next `C3D_FrameBegin` waits forever. That
  is the likely cause of the hang when starting the queue mid-frame was first tried. citro3d only
  stops it once it's idle, at the next frame's start.
* **Texture copies with 4 KB rows froze the GPU.** Use dimensions of 0 for a contiguous copy.
* **A replaced texture stayed bound.** Once a texture is freed, clear it from the texture units, or
  the next draw hands the GPU freed memory.
* **One copy of the screen for both eyes.** The game copies the screen once and draws the copy for
  as long as the pause menu is open. With one texture, the right eye's copy overwrote the left's,
  and the menu jumped when it opened. Each eye keeps its own.
* **Culling at the screen's edge.** The interpreter drops triangles just off screen. A shifted eye
  can see past the edge, so the test widens by the eye shift.

## Where the time goes now

At the Freezeezy spot, per 16.6 ms frame:

| Work | 2D | 3D | Core |
|---|---|---|---|
| Interpreter (render thread) | about 9 ms | about 9.5 ms | 0 |
| Replay | 1–4.6 ms | about 7.4 ms | 2 |
| GPU | 4–8 ms | about 7–10 ms, alongside the replay | GPU |
| Game logic | about 3 ms per update | about 3 ms per update | 2 |

The render thread spends the rest of each frame waiting for vblank.

### Not tried yet

* **Particle thinning,** from rt64-3ds's
  [porting notes](https://github.com/oneup03/rt64-3ds/blob/HEAD/docs/porting-a-game.md) (the
  renderer behind BanjoRecomp3D). One Jiggy's sparkles were 225 of a frame's 360 draws.
* **The right eye's CPU work.** It repeats the left eye's scan of each batch for varying colours
  (about 11% of the replay) and its state setup. Keeping the scan's results would save about 1 ms.

The rt64-3ds notes also found that **indexed triangles saved no time**, and that **BK's own culling
and low-detail models must stay on** (Lighthouse's defaults).

## Old 3DS

On an Old 3DS (two cores, 268 MHz, no L2 cache), the game logic and the replay fall back to core
0, and interpolation is off. A New 3DS made to act like an Old one runs at about 10 fps, in slow
motion. See the [roadmap](ROADMAP.md#old-3ds-speed).

## Startup

From about 15.8 s to about 2.5 s:

| Change | Saved |
|---|---|
| Read archives through one handle and a 64 KB block cache (each file-system read costs milliseconds, and libzip reads in small pieces) | 15.8 → 2.8 s |
| Open `bk.o2r` once: every extra open parses the whole file list again | 3.4 s per extra open |
| Don't rewrite settings when nothing changed (one SD write took 1.1 s, and startup saved six times) | Up to 6.7 s, intermittent |
| Don't let SDL mount a romfs the app doesn't have | 1.0 s |

What's left: the `bk.o2r` file list (about 0.9 s) and 630 separate sound reads (about 0.76 s).
Cutting those would mean changing the archive format.

## Making `bk.o2r` on the 3DS

Since v1.2 the app can make `bk.o2r` from a ROM on its first start, with Torch, Lighthouse's asset
extractor, built into the app. Unchanged, Torch needed far more memory than a 3DS has and would
have run for over 15 minutes. All the archives below are identical, file for file, to the ones the
PC makes, for all four ROM versions.

### Memory

A `.3dsx` gets about 89 MB of heap on a New 3DS. Torch peaked at about 740 MB on a PC, because it
kept everything until the end:

| Change | Peak (PC) |
|---|---|
| Unchanged Torch | 741 MB |
| Write each asset to the archive as soon as it is parsed, then free it, instead of parsing all ~3,500 first | 166 MB |
| Free each asset's decompressed data once it is written, skip the up-front decompression of every asset, and drop the ROM copy the cartridge header reader kept | 121 MB |
| Don't keep a copy of every exported file for the C-code export, which a binary export never reads | 76 MB |
| Copy only the asset table (28 KB) to read it, not the rest of the ROM | 60 MB |

The peak on the 3DS is 48 MB, including the 16 MB ROM. Writing the zip straight to the file instead
of building it in memory hardly changed the peak, but it mattered for speed (below).

### Time

| Run, on a New 3DS (Torch in a test app) | Time |
|---|---|
| First working run: the archive written through the C library's 1 KB buffer | 963 s |
| Writes gathered into a 1 MB memory window | 576 s |
| Writes in whole 1 MiB blocks at 1 MiB-aligned offsets | 198 s |
| The same, with the writes thrown away | 193 s |
| Uncompressed, writes thrown away | 141 s |
| **In the game: uncompressed, with logging turned down** | **about 80 s** |

Reading the 16 MB ROM through a C++ stream iterator, one byte at a time, took 30.5 s. One `fread`
into a buffer of the right size takes 1.5 s.

### SD card writes

The slow part was the SD card, and only for one kind of write:

| Writing 26 MB on a New 3DS | Speed |
|---|---|
| 1 MiB blocks into a file that grows | 2.9–3.9 MB/s |
| 1 MiB blocks into a file whose size is set first | 5.6–6.4 MB/s |
| 1,032,486-byte blocks, each after a seek, as the zip writer made them | 0.10 MB/s |

Two things made the archive writes slow:

* **Tiny writes.** miniz writes each file's data, then seeks back to fill in its header. newlib
  flushes its buffer on every seek, so a bigger stdio buffer does nothing: the 26 MB archive still
  went out as tens of thousands of writes of 30 bytes to 30 KB, and each is a request to the file
  system service.
* **Unaligned writes.** Even in 1 MB pieces, writes that don't start and end on a block boundary
  are about 50 times slower.

Torch's zip writer now keeps the archive in memory from the last written block on, and writes only
whole aligned 1 MiB blocks, once nothing in them can change. The only unaligned write is the end of
the file. The archive's writes now take about 4 s in all.

### What's left

* **Torch's own parsing takes about 140 s of CPU time in the test app.** It's spread across
  Torch's whole design: assets pass through YAML nodes, which allocate and adjust reference counts
  at every step, and allocation is far slower on the 3DS than on a PC. A PC does the same work in
  about 1.7 s, more than the CPU speed alone would explain. Getting under a minute would mean
  rewriting the Banjo-Kazooie path in Torch without YAML.
* **Compression costs about 50 s**, so the 3DS stores files uncompressed: 47 MB instead of 26 MB.
  The game reads either kind. Compressing on another core during the parse would keep the smaller
  file at little cost, but adds code.
* Each time spdlog's pattern changes, it rebuilds its formatter. Torch did that twice per asset,
  even with those log lines turned off. It now only does that when they are on.

### Measuring

`tools/3ds/extract-test` is a small app that runs only the extraction, from
`sdmc:/3ds/lighthouse/extract-test/` (`rom.z64`, `config.yml`, `torch.hash.yml` and `assets/`). It
reports the time, the peak heap, and every SD card call with its count and time, and can throw
the archive's writes away to time the CPU work alone. Its **Y** option measures the SD card's
write speeds in under a minute.

Luma's Rosalina menu has a GDB server (Debugger options → Enable debugger). Attaching
`arm-none-eabi-gdb` to `192.168.1.x:4000` every few seconds, and recording each thread's stack,
works as a rough profiler. Backtraces stop at libctru's wait calls; `x/64a $sp` shows the callers.
