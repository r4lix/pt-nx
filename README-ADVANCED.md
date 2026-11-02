# pt-nx: details for advanced users

The short version is [README.md](README.md). This page has the details: what was tested, optional files, the clock
settings, where everything goes on the SD card, performance, building from source, credits and licences.

pt-nx is a port of [LoreanXavier's P.T. PC Port](https://github.com/LoreanXavier/pt-pc), the native re-implementation of P.T.
in C++ and Vulkan. It runs from the homebrew menu on a console with Atmosphère and renders through
[nxvk](https://github.com/PalindromicBreadLoaf/nxvk), Mesa's NVK Vulkan driver for the Switch's Tegra X1.

## Status

- Played through to the ending on a Switch Lite (Atmosphère 1.10.2, firmware 21.2.0, Horizon OC), handheld.
- Not tested: docked play, the original Switch (V1), the OLED model.
- Based on the PC port's 1.0 release (October 2026). Fixes from the PC port's later releases (1.0.1 to 1.0.8) are not in
  this build yet; a report about something the PC port has since fixed is still welcome, but may be known.

## Requirements

- A Switch running **Atmosphère** with the homebrew menu.
- **Your own P.T. dump** (US release, CUSA01127): a dump folder from your console, or the three archives the PC port's
  setup copies from a dump or a fake PKG. A store PKG cannot be used.
- **A clock manager with an overclock**: Horizon OC, or sys-clk with an overclocking KIP (see
  [the clock settings](#overclock-and-the-automatic-clock)).
- Optional, for frame generation (30 -> 60 fps): your own `Lossless.dll` from the Lossless Scaling application. Its shaders
  are read from that file at run time; nothing of it is included.

## Install

1. Extract `pt-nx.zip` to the root of the SD card. It holds `switch/pt.nro` and the `switch/pt/` folder (the licence texts
   and the folder for your game files).
2. From your dump, copy `chunk1.psarc`, `texture.qar` and `pathid_list_ps4.bin` to `/switch/pt/game/CUSA01127/`.
   FAT32 and exFAT cards both work: the largest file is under 1 GB.
3. Optional: copy `sce_sys/pic1.png` from your dump to `/switch/pt/game/CUSA01127/sce_sys/pic1.png` to see the PS4 title
   picture while the game loads.
4. Optional: copy `Lossless.dll` to `/switch/pt/lsfg/Lossless.dll` and turn on frame generation in the Minus menu.
5. Start the homebrew menu in **title takeover mode** (hold **R** while starting any game from the HOME menu), then start
   P.T. Started from the album (applet mode), homebrew gets too little memory.

## Files on the SD card

| Path | What it holds |
|---|---|
| `/switch/pt.nro` | the game |
| `/switch/pt/game/CUSA01127/` | your game files, and the optional `sce_sys/pic1.png` |
| `/switch/pt/lsfg/Lossless.dll` | optional, for frame generation |
| `/switch/pt/save/` | settings (`pt.ini`), saves, `pt.log`, the compiled shader cache (`pipelines/`, safe to delete) |
| `/switch/pt/switch.ini` | the Minus menu's settings |
| `/switch/pt/photos/` | pictures from the photo mode |
| `/switch/pt/licenses/` | the licence texts |

`pt.log` is the first thing to look at when something goes wrong, and the file to attach to a bug report.

## Overclock and the automatic clock

The game was tuned with the GPU at **1152 MHz** and the memory at **1600 MHz or more**: the memory clock matters as much as
the GPU clock here. There are two ways to get there:

- **Set the clocks in your clock manager.** The game's default (**System** under **GPU clock** in the Minus menu) leaves
  the clocks to it.
- **Or let the game raise them**: choose **Automatic** under **GPU clock**. While frames are too slow it raises the GPU, the
  CPU and the memory, up to its ceilings; your clock manager's own setting stays the floor, and it only goes as far as the
  clock manager allows. Raise the **Automatic ceiling** to 1152 MHz: it starts at 768 MHz, the console's own docked
  maximum. The CPU ceiling starts at 1785 MHz, the console's own boost rate.

A fixed GPU rate can also be picked in the same menu.

## Controls and menus

The game plays like the PS4 original, with the Switch's own convention: **A** confirms and interacts, **B** goes back.

- **"Jack"**: when the game wants you to say "Jack", press **ZL + ZR together**. The Switch has no microphone, so this
  replaces speaking.
- **Plus (+)**: the game's pause menu. Its **PC Settings** page holds the port's game options and **Extras**: the loop
  browser and the museum (after finishing the game once), the free camera and the photo mode.
- **Minus (-)**: the port's menu: fps and resolution display, 30 fps lock, render size, frame generation, the clock
  settings (System / Automatic / a fixed GPU rate) and graphics options.

## Performance and help wanted

Measured on a Switch Lite in the heaviest corridor, 1024x576 with frame generation:

| Clocks (GPU / memory) | Scene GPU time per frame | Frame rate |
|---|---|---|
| 307 / 1331 MHz (stock handheld) | about 60 ms | 12-15 fps |
| 1152 / 1331 MHz | about 27 ms | 30 fps, little headroom |
| 1152 / 1600 MHz (Automatic with its ceiling at 1152) | lower again: the memory clock alone was worth ~8% | steady 30 fps |

A higher memory overclock helps further (about 24 ms in an earlier build on a Horizon OC profile with memory well above
1600 MHz).

What is already done for the console: half-resolution lighting, quarter-resolution light probes in one pass culled per
subgroup, half-resolution particles, early-Z G-buffer pipelines, a shadow cache, models behind the level's occluders
skipped, a persistent pipeline cache, LSFG frame generation, and the automatic GPU / CPU / memory clock.
[switch/README.md](switch/README.md) has the detailed breakdown, the measurements and the test tools. The biggest costs left
are the light resolve pass, the forward and particle passes and the G-buffer. Profiling ideas, shader work and pull
requests are welcome.

## Building from source

See [switch/README.md](switch/README.md): devkitPro (devkitA64, libnx, portlibs), nxvk built with Docker, and the Vulkan
SDK (or Docker) for the shaders.

## Credits and licences

- **LoreanXavier**, the P.T. PC Port this is built on (MIT). The original README is [README-PC.md](README-PC.md).
- **nxvk** by PalindromicBreadLoaf: Vulkan on the Switch (Mesa NVK; new files GPL-2.0-or-later, Mesa files under their own
  licences). The release was built with nxvk revision `556cc2e`.
- **lsfg-vk** (GPL-3.0-or-later), the frame generation backend, here as used by NetherSX2_nx.
- Everything else compiled into the `.nro` is listed in [switch/THIRD_PARTY.md](switch/THIRD_PARTY.md).

The port's own code is MIT, like the PC port. nxvk and lsfg-vk are linked statically into the `.nro`, so the `.nro` as a
whole is distributed under the GPL (version 3); this repository is its complete source.

P.T. is © Konami. This is a fan project, not affiliated with or endorsed by Konami, and it contains no game content.
