# pt-nx

P.T. (Silent Hills) running natively on the Nintendo Switch, a port of [LoreanXavier's P.T. PC Port](https://github.com/LoreanXavier/pt-pc).

**No game content is included.** You need your own copy of the game (P.T. for PS4, US release CUSA01127).

**An overclocked console is needed** for a playable frame rate. Help to make it run faster is very welcome: see
[performance and help wanted](README-ADVANCED.md#performance-and-help-wanted).

## Disclaimer

This Switch port was created **entirely with the help of AI coding tools**.

I am **not a developer**, so the code may contain bugs, security vulnerabilities, mistakes, or unfinished features. pt-nx is a personal project that I decided to share with others who may find it useful.

Use it at your own risk. I cannot guarantee support, stability, compatibility, security, or regular updates.

## Install

1. Download `pt-nx.zip` from the [latest release](https://github.com/r4lix/pt-nx/releases/latest) and extract it to the root of your SD card. This creates `sdmc:/switch/pt.nro` and `sdmc:/switch/pt/`.
2. Copy `chunk1.psarc`, `texture.qar` and `pathid_list_ps4.bin` from your own dump of the game to `sdmc:/switch/pt/game/CUSA01127/`.
3. Overclock the console with your clock manager (Horizon OC, or sys-clk with an overclocking KIP): GPU around 1152 MHz, memory 1600 MHz or more.
4. Launch **P.T.** from the homebrew menu in title takeover mode (hold **R** while starting any game from the HOME menu).

Saves are written to `sdmc:/switch/pt/save`.

## Controls

- **A** confirms and interacts, **B** goes back.
- **ZL + ZR** together say "Jack" (the Switch has no microphone).
- **Minus (-)** opens the port's menu: resolution, frame generation, clocks.

Optional files, frame generation, the automatic clock, performance and building from source: [README-ADVANCED.md](README-ADVANCED.md).

P.T. is © Konami. This is a fan project, not affiliated with or endorsed by Konami.
