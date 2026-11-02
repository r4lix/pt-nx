# Third-party code in the Switch build

What `switch/CMakeLists.txt` compiles or links into `pt.nro`, with its licence and source. The port's own code is MIT
(`LICENSE`). Because nxvk (GPL-2.0-or-later) and lsfg-vk (GPL-3.0-or-later) are linked statically, the `.nro` as a whole is
distributed under the GPL version 3 (`third_party/lsfg-vk/LICENSE.md` holds its text); this repository is its complete source.

| Component | Licence | Source |
|---|---|---|
| nxvk (Mesa NVK for the Switch), `libnvk.a`, `libnvk_support.a` | new files GPL-2.0-or-later, Mesa files MIT and others (see each file's SPDX tag) | https://github.com/PalindromicBreadLoaf/nxvk, revision `556cc2e` |
| lsfg-vk (frame generation backend) | GPL-3.0-or-later | `third_party/lsfg-vk` |
| libnx | ISC | https://github.com/switchbrew/libnx |
| Vulkan-Headers | Apache-2.0 / MIT | https://github.com/KhronosGroup/Vulkan-Headers |
| volk | MIT | https://github.com/zeux/volk |
| Vulkan Memory Allocator | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| GLM | MIT | https://github.com/g-truc/glm |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| stb | MIT / public domain | https://github.com/nothings/stb |
| Lua 5.1 | MIT | https://www.lua.org |
| libogg, libvorbis | BSD-3-Clause | https://github.com/xiph/ogg, https://github.com/xiph/vorbis |
| bc7enc_rdo | MIT / public domain | https://github.com/richgel999/bc7enc_rdo |
| Expat | MIT | https://github.com/libexpat/libexpat |
| HarfBuzz | MIT ("Old MIT") | https://github.com/harfbuzz/harfbuzz |
| FreeType | FreeType Licence (FTL) | https://freetype.org |
| libpng | libpng licence | http://www.libpng.org |
| bzip2 | bzip2 licence (BSD-like) | https://sourceware.org/bzip2 |
| zlib | zlib licence | https://zlib.net |
| Noto fonts (in the .nro's romfs) | SIL Open Font License 1.1 | `assets/fonts` (licence texts beside the fonts) |

Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.

The frame generation shaders are not included: they are read at run time from the player's own `Lossless.dll` (Lossless
Scaling). No file of P.T. is included either: the game's archives, and optionally its title picture, come from the player's
own dump.
