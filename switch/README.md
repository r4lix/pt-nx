# P.T. for the Nintendo Switch (homebrew)

A port of this engine to a Switch running custom firmware and the homebrew menu. It is played on a Switch Lite and holds a steady 30 fps
in the heaviest corridor at 1024x576 with the console overclocked (see Performance); the player-facing pages are `README-SWITCH.md` (the short one, `README.md` in the release repository) and `README-ADVANCED.md`.

It keeps the engine's Vulkan renderer and gets Vulkan on the console from
[nxvk](https://github.com/PalindromicBreadLoaf/nxvk) (Mesa's NVK driver for the Tegra X1), linked statically into the `.nro`.

## What is different from the desktop build

| Area | Switch build |
|---|---|
| Platform layer | `src/platform/switch/`: an SDL3 compatible layer on libnx (pad, audio, window/Vulkan surface, SD paths), `dlfcn.h`, ImGui backend |
| Frame generation | LSFG (lsfg-vk) with the player's own `Lossless.dll`, see below; the PC port's DLSS / FSR / XeSS frame generation is not available |
| Not available | DLSS / FSR / XeSS, Streamline, OpenXR, voice recognition (Whisper), virtual pads, updater (HTTP), LiveSplit, child processes (`switch_stubs.cpp` makes each report "not available") |
| Ray tracing | off: nxvk has no ray query, the engine's raster path is used |
| First run | the **Low** graphics preset; the window is the screen size (1280x720 handheld) and the 3D scene renders at a smaller, adaptive size that is scaled up |
| Input | the pad only. A confirms and interacts, B goes back, as on the console (`sdl_switch.cpp` reports A in the engine's Cross role and B in its Circle role, prompts included); X and Y keep their places; ZL/ZR are the triggers; **ZL + ZR together say "Jack"** in the final puzzle (no microphone); HD rumble is used. The PC Settings, Extras, museum and photo mode hints name the console's buttons and SD folders instead of PC keys and PlayStation buttons (`src/game/ui/pc_settings_switch.h`, all 12 languages) |
| Shaders and fonts | compiled SPIR-V and the fonts are **inside the .nro** (romfs), nothing to copy for them |

## Files on the SD card

```
sdmc:/switch/pt.nro                         the game (from build/switch/pt.nro)
sdmc:/switch/pt/game/CUSA01127/             YOUR dump: chunk1.psarc, texture.qar, pathid_list_ps4.bin
sdmc:/switch/pt/save/                       settings, saves, log (created on first run)
sdmc:/switch/pt/save/pipelines/<nro>.bin    the compiled shader pipelines, kept between runs (see below; safe to delete)
sdmc:/switch/pt/switch.ini                  the Switch options saved from the in-game menu (see below)
sdmc:/switch/pt/photos/                     pictures from the photo mode (PC Settings > Extras), 1280x720 PNG
```

* The game files are **not** part of this repository and are not built here: copy your own dump (the same folder the desktop
  port reads) to the path above.
* FAT32 and exFAT cards both work: `chunk1.psarc` (422 MB) and `texture.qar` (892 MB) are under the 4 GB FAT32 file limit.
* Optional: `sce_sys/pic1.png` from the dump, copied to `sdmc:/switch/pt/game/CUSA01127/sce_sys/pic1.png`, is shown while the game
  loads (the PS4 title picture). The .nro shows the game as the original did: P.T.'s title art as its icon (`switch/assets/icon.jpg`,
  via SteamGridDB), "P.T." by 7780s Studio. A personal build can also carry the title picture as `cover.jpg` in `switch/assets/private/`
  (kept out of the repository); `pt-public.nro` (`ninja pt_public_nro`) is packed from the same program without it, for releases.
* Start the game with the homebrew menu in **title takeover mode** (hold **R** while launching a game from the home menu, then pick it).
  Launched from the album ("applet mode"), the homebrew only gets a fraction of the RAM.
* `sdmc:/switch/pt/save/pt.log` is the first place to look when it does not start. With `nxlink -a <console ip> -s pt.nro` the log
  streams to the PC instead.

## In the game: the Minus menu

**Minus (-)** opens the engine's settings window (the game pauses; D-pad moves, A activates). Its first section, "Switch: performance and
debug", has:

* live fps, scene GPU time, render size, GPU and CPU clocks, per-pass times;
* **Show fps and resolution**: fps, scene size and GPU time in the top left corner;
* **Lock 30 fps**: every frame stays on screen two VBlanks (a steady 30 fps). Off, the resolution controller aims for 60 fps;
* **Dynamic resolution** and the scene GPU budget (locked mode): the scene render size follows the measured GPU time; or pick a fixed size;
* GPU clock request (System / Automatic / a fixed rate). **Automatic** raises the clock only while frames miss the budget (straight to the
  rate predicted to fit, up to `max_mhz`) and lowers it again after a few seconds of headroom, at most one change every ~1.5 s. It never goes
  below the clock the system or the clock manager gives, and reads the clock back every half second: when that setting changes while playing
  (switched off in the overlay, say), Automatic follows it and takes over from there. While it holds the GPU above that floor it keeps the
  memory at 1600 MHz at least; back at the floor it hands both back. **Automatic also runs the CPU clock** the same way, from the game
  thread's share of each frame (the frame interval minus its waits for the GPU and the display): over 80% of the frame, or a frame over
  three frame times (a stage load, pipeline builds, which go straight to the ceiling), raises it; 3 s with room steps it down. Its ceiling
  is `cpu_max_mhz` (default 1785 MHz, the console's own boost rate, no overclock needed). Switch Lite, hallway, 1024x576 with frame
  generation, everything at stock to begin with: GPU 1152, memory 1600, CPU 1122 MHz in play (the game thread's ~16 ms a frame barely
  changes with the CPU clock there), 1785 MHz during loads, a steady 30 fps. With a clock
  manager running (Horizon OC's `hoc:clk`, sys-clk's `sysclk:ipc`) the game asks it for a temporary override instead of setting the clock
  itself, and clears the override when it exits or when "System" is picked. 4000 changes between 307 and 1152 MHz in 27 minutes through the
  override caused no problem (Switch Lite, Horizon OC, firmware 21.2.0). Without a manager the game sets the clock through clkrst and puts it
  back if the system changes it;
* **The `pcv` crash (error 2011-0102, program 010000000000001A) of earlier builds** came from the game leaving its clock-service sessions open
  when it quit: a .nro started from the homebrew menu, or chained to the next one, runs in the same process as the menu, so every launch
  added a few sessions until `pcv` ran out of session memory (the 3rd launch in a row on a Switch Lite). The game now closes them at exit
  (five launches in a row, no crash). With an older build, close the homebrew menu from HOME between two launches;
* render toggles for finding what costs time: lights, probes, shadows, AO, reflections, bloom, DOF, motion blur, grain, distortion,
  subsurface, atmosphere, FXAA, mirrors, **Half-res lights**, **Quarter-res probes**, **Probes in one pass**, **Probe culling per subgroup**,
  **Skip models behind occluders**, **Half-res effects**, particle effects;
* **Frame generation (LSFG, 30 -> 60 fps)**: every rendered frame is shown after one generated between it and the previous one, so the screen
  gets 60 images a second from a 30 fps game. It needs **your own copy of `Lossless.dll`** (from the Lossless Scaling application) at
  `sdmc:/switch/pt/lsfg/Lossless.dll`: its frame-generation shaders are read from it at run time and nothing of it is shipped here. The generator
  (`third_party/lsfg-vk`, GPL-3.0-or-later, as used by NetherSX2_nx) costs about 3 ms a frame at 960x540 with the default motion flow of 2.0, keeps
  the 30 fps lock and a fixed scene size, adds one VBlank of delay and can smear fast motion. The first start with it builds its pipelines (several
  seconds), later starts take a fraction of a second. Off by default; **Motion flow** (higher = cheaper, coarser) applies at the next start.
* **Fidelity preset**: the game's Original (PS4) graphics preset with full-resolution lights, probes and effects, no sharpening, full-size textures
  and a 1280x720 scene, without the 30 fps lock (about 10-15 fps in the busiest rooms); the same button returns to the performance preset.
* **Save Switch settings** writes `sdmc:/switch/pt/switch.ini` (`lock30`, `size`, `budget_ms`, `dynamic`, `overlay`, `effects`, `gpu_mhz`,
  `max_mhz`, `auto`, `frame_gen`, `lsfg_flow`, `fidelity`).

## Performance

Measured on a Switch Lite at a 1152 MHz GPU clock, in the heaviest area found so far (a hallway with about 20 lights and 63 probes):
a steady 30 fps at about 832x468 with the lock, 60 fps at 448x252 unlocked. What got it there (each toggleable, see the Minus menu or
the environment variables below): one-pass lighting with a light list and per-light data in a uniform buffer, half-resolution lights,
quarter-resolution probes, half-resolution particle effects, early depth testing for opaque G-buffer draws plus front-to-back sorting,
a per-view shadow cache, and a log that is flushed once a second instead of after every line (a flush per line was an SD-card write
that stalled a frame every 10 s). Note: GPU timestamps on the Switch are corrected by 31.25/19.2 (NVK reports the desktop
timer period; the Tegra X1 timer runs from the 19.2 MHz crystal), see `vk_context.cpp`.

Where the time goes (Switch Lite, the hallway, 1024x576 with frame generation, GPU 1152 MHz, memory 1331 MHz, 2026-11-01): G-buffer 5-6 ms,
lighting 9.4 ms (probes 0.6, half-resolution lights 5.5, resolve 3.3), compose 7-8.5 ms (forward 3.8-4.3, effects 3.5-4.1), post 2.4-2.6 ms,
shadows 1.2-1.7 ms; the log prints the lighting split every 10 s ("lighting pass:"). **The memory clock counts as much as the GPU clock**: the
GPU shares the console's memory, and handheld mode runs it at 1331 MHz. At 1152 MHz GPU the same view took 30.5 ms of scene GPU time at
1331 MHz memory and 28.0 ms at 1600 MHz (the docked rate); the overclocked memory of a Horizon OC setup took it to about 24 ms. Raise the
memory together with the GPU in the clock manager; the game's Automatic clock keeps it at 1600 MHz or more while it raises the GPU (never
lowering a higher setting). The half-resolution light shader works in world space since 2026-11-01 (no per-light matrix transforms): 6.4 -> 5.5 ms.

The probes (baked spherical-harmonic light in boxes, 63 in the hallway, 51-64 of them in view) were drawn as one box each, and every pixel a
box covered fetched depth and normal and evaluated the probe four times (the bilinear 16x16 sphere map of the original). Since 2026-11-01 one
full-screen pass takes the list of probes in view (`probe_list.frag`): depth, normal and the sphere-map setup once per pixel, nine
multiply-adds per probe that reaches it. On its own that pass is not always faster (every pixel tests every listed probe: 1.6 ms against 2.8
in one hallway view, slower in another), so each subgroup of 32 pixels also takes the world box its pixels span, its lanes test the listed
probes against it side by side, and it walks only the probes whose boxes meet it (`probe_list_sg.frag`, used when the driver has subgroup
operations in fragment shaders; NVK has). Hallway: probes 2.8 -> 0.6 ms, scene GPU time 29.8 -> 27.6 ms, the same image (A/B screenshots,
also with the probe light alone, `PT_DEBUG_VIEW=12`, and at 960x540, whose quarter-size target has an odd height).

Models hidden behind walls are not drawn since 2026-11-01. The levels carry occluders (`OccluderEx` entities: the walls the original engine
culled its rendering with, switched on and off by the game's scripts); the port already skipped lights behind them, and now skips models whose
bounds lie wholly behind one, in the camera views only (a model out of sight can still cast a shadow into view, so the shadow maps keep
everything). Hallway, 1024x576, GPU 1152 MHz: looking down the corridor 60 of the 186 models in front of the camera (75,000 triangles) are
behind occluders, G-buffer 5.3 -> 4.7 ms, scene 28.6 -> 27.95 ms; facing the wall at the end of the test walk 63 of 126, G-buffer 5.7 -> 4.5
ms, scene 27.65 -> 26.7 ms. Screenshots with and without differ only by the usual frame-to-frame noise. The log prints the counts every 10 s
("models:").

Compiled pipelines are kept in `sdmc:/switch/pt/save/pipelines/` (one file per .nro, checked against the .nro's size and date and a checksum),
so later starts skip NVK's compiles; the G-buffer's material variants, which appear while playing, are built on a worker thread instead of
in the frame (5-35 ms each on the console's CPU).

## Test aids (environment variables)

Set them from `romfs:/test_env.txt` inside an experiment .nro (one `NAME=VALUE` per line; `render_size=WxH` pins the scene size).
`PT_AUTOWALK=<start s>,<walk s>,<-1|1>,<turn s>` makes the game walk itself (for unattended runs), `PT_START_FLOOR=f010` fixes the start,
`PT_CYCLE=1` switches probes/lights on and off in turns, `PT_SHADER_STATS=<shader list|*>` logs NVK register counts and the compiled
assembly, `PT_SIZE_CYCLE=512x288,960x540,...` sweeps sizes, `PT_LOCK30=0`, `PT_OVERLAY=1`, `PT_LIGHT_FULL=1`, `PT_PROBE_FULL=1`,
`PT_VFX_FULL=1`, `PT_GBUFFER_EARLY=0`, `PT_GBUFFER_VARIANTS=0`, `PT_SHARPEN=0`, `PT_SHADOW_CACHE=0`, `PT_PROBE_CULL=0` turn single optimisations off for A/B runs.
`PT_PIPELINE_CACHE=0` (no pipeline cache), `PT_GBUFFER_ASYNC=0` (G-buffer variants built in the frame), `PT_PROBE_LIST=0` (one box per probe),
`PT_MODEL_OCCLUSION=0` (models behind occluders drawn too) likewise. Clocks for a run: `PT_GPU_MHZ=<n>`
(a fixed GPU clock), `PT_MEM_MHZ=<n>` (memory), `PT_AUTO_CLOCK=1`, `PT_GPU_MAX_MHZ=<n>`, `PT_CLOCK_OUTSIDE=<MHz>,<s>` (a GPU clock set at
start as if by the clock manager, handed back at `<s>`: Automatic has to notice and take over), `PT_CPU_OUTSIDE=<MHz>` (a CPU clock set at
start the same way: Automatic's CPU floor), `PT_CPU_MAX_MHZ=<n>`; sweeps that log the GPU time per step (12 s each from
`<start s>` of log time on): `PT_MEM_CYCLE=<start s>,<MHz>,...` (memory clocks, 0 = the one at start, put back afterwards) and
`PT_LIGHT_CYCLE=<start s>,<variant>,...` with `PT_LIGHT_VARIANTS=<suffix>,...` (extra `light_all_half<suffix>.frag` /
`light_all_half_lean<suffix>.frag` builds carried in the test .nro; a `b` after a variant draws the probes as boxes, a `p` as the one-pass list
without the subgroup culling, an `o` draws the models behind occluders too). `PT_QUIT_AFTER=<s>` and `PT_NEXT_NRO=<sdmc path>` end a run by itself and
start the next .nro (unattended runs through the SwitchLink app).

## Building

Requirements: devkitPro with `switch-dev` (devkitA64, libnx) and the portlibs `switch-freetype`, `switch-harfbuzz`, `switch-zlib`,
`switch-libpng`, `switch-bzip2`; CMake 3.28+, Ninja, git; Docker for nxvk (and for the shaders unless the Vulkan SDK is installed).

1. **nxvk** (one time, long: it builds Mesa's driver and a Rust sysroot). From a checkout of nxvk:
   `docker build -t nxvk switch/docker`, then run its Makefile's `package` target inside the image with `CONTAINER=` empty
   (`docker run --rm -v <volume>:/work -w /work nxvk make CONTAINER= package`). Build in a Docker volume, not a bind mount of a
   Windows folder (slow, and symlinks break). The result is `switch/build/pkg/lib/libnvk.a` and `libnvk_support.a`; put them in
   `<nxvk package>/lib` together with nxvk's `include/vulkan` and `include/vk_video` as `<nxvk package>/vulkan`, `/vk_video`.
2. **Configure and build** (from the repository root, in a shell with devkitPro on `PATH`; pass `-DPT_NXVK_DIR` as an msys path
   such as `/home/<user>/nxvk-pkg` when using devkitPro's msys):
   ```
   cmake -G Ninja -S switch -B build/switch -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release -DPT_NXVK_DIR=<nxvk package>
   ninja -C build/switch
   ```
   This downloads the third party sources the desktop build also uses (volk, VMA, glm, ImGui, stb, Lua 5.1, ogg/vorbis, bc7enc)
   plus Vulkan-Headers and libexpat. The output is `build/switch/pt.elf`, `pt.nro` (shaders and fonts inside it).
3. **Shaders**: with `glslc` on `PATH` (or `VULKAN_SDK` set) the build compiles `shaders/*` itself. Without it, use the nxvk Docker image,
   which has glslang and spirv-opt: `python switch/tools/inline_shaders.py shaders <flat dir>` (glslang has no `#include`), then run
   `switch/tools/compile_shaders.sh` in the image (the usage is in the script) into `build/switch/romfs/shaders`, and repack the .nro
   with `elf2nro ... --romfsdir=build/switch/romfs` (ninja does not repack when only the compiled shaders changed). The 1.0.0 release's
   shaders are all compiled by the build with glslc from the Vulkan SDK 1.4.363 (glslang 16.6, `-O`); on a Switch Lite they ran within
   0.1 ms a frame of the earlier glslang 12 binaries, with the same image. A different compiler version gives different SPIR-V (one
   light shader measured 2% apart between versions), so compare on the console when changing it.

## Known gaps

* Played through to the ending on a Switch Lite (handheld); docked play and other console models are not verified yet. nxvk itself calls its output a "broken Vulkan
  driver" (the build sets `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER`).
* The 3D scene renders at a reduced size (adaptive) and is scaled up; the game's own UI and the menus are drawn at the screen size.
* Docked/handheld changes while running are not handled (the size is read once at start).
* The touch screen, gyro and the second player are not mapped. Only one pad (handheld or P1) is read.
* Sound goes through `audout` at 48 kHz stereo; the engine's output rate is assumed to be 48 kHz.
* Based on the PC port's 1.0 release: the fixes of its 1.0.1-1.0.8 releases are not merged (the PC port has since moved its renderer
  behind an abstraction layer for DirectX 12, so a merge means porting this Switch work onto that layer).

## License

The port code is MIT like the rest of this repository. **nxvk is GPL-2.0-or-later and is linked statically**, so a binary that includes
it is a combined work under the GPL: whoever receives such a binary must be able to get this source (see nxvk's `switch/docs/LICENSING.md`).
lsfg-vk (frame generation) is GPL-3.0-or-later and linked statically too, so a `.nro` is distributed under the GPL version 3 as a whole.
`switch/THIRD_PARTY.md` lists every component compiled into the `.nro` with its licence.
