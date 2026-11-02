# Nintendo Switch homebrew build of the P.T. port (a separate project, so the desktop build in ../CMakeLists.txt stays as it is).
#
#   cmake -G Ninja -S switch -B build/switch -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DPT_NXVK_DIR=<nxvk package>
#   cmake --build build/switch            -> build/switch/pt.nro
#
# Graphics: the renderer is Vulkan, provided on the console by nxvk (NVK for the Switch, github.com/PalindromicBreadLoaf/nxvk),
# linked statically; PT_NXVK_DIR holds its package (lib/libnvk.a, lib/libnvk_support.a, vulkan/ headers). nxvk is GPL-2.0-or-later,
# so a binary built here that links it is a combined work under the GPL (switch/README.md).
cmake_minimum_required(VERSION 3.28)
project(pt_switch LANGUAGES C CXX)

if(NOT PT_SWITCH)
  message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake (path relative to the repository root)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(PT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(PT_NXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nxvk-pkg" CACHE PATH "The nxvk package: lib/libnvk.a, lib/libnvk_support.a, vulkan/, vk_video/")
set(PT_NXVK_SRC "${PT_ROOT}/../nxvk" CACHE PATH "The nxvk source (switch/smoke/nvk_compat.c supplies newlib gaps the driver needs)")
option(PT_RELEASE_LOCKS "Release game: the loop browser stays locked until the game was finished once" ON)

include(FetchContent)
cmake_policy(SET CMP0169 OLD)
set(FETCHCONTENT_QUIET ON)

# Vulkan headers matching volk, from the Khronos repository; nxvk's own headers come after them on the include path
FetchContent_Declare(vkheaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG v1.4.350 GIT_SHALLOW TRUE)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG 1.4.350 GIT_SHALLOW TRUE)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.3 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b-docking GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_Declare(lua51 URL https://www.lua.org/ftp/lua-5.1.5.tar.gz
  URL_HASH SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333)
FetchContent_Declare(ogg GIT_REPOSITORY https://github.com/xiph/ogg.git GIT_TAG v1.3.5 GIT_SHALLOW TRUE)
FetchContent_Declare(vorbis GIT_REPOSITORY https://github.com/xiph/vorbis.git GIT_TAG v1.3.7 GIT_SHALLOW TRUE)
FetchContent_Declare(bc7enc URL https://codeload.github.com/richgel999/bc7enc_rdo/zip/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0)
FetchContent_Declare(expat GIT_REPOSITORY https://github.com/libexpat/libexpat.git GIT_TAG R_2_6_4 GIT_SHALLOW TRUE)
foreach(dep vkheaders volk vma glm imgui stb lua51 ogg vorbis bc7enc expat)
  FetchContent_Populate(${dep})
endforeach()

# ---- third party code -------------------------------------------------------------------------------------------------------

file(GLOB LUA51_SOURCES ${lua51_SOURCE_DIR}/src/*.c)
list(FILTER LUA51_SOURCES EXCLUDE REGEX ".*/(lua|luac|print)\\.c$")

add_library(pt_thirdparty STATIC
  ${volk_SOURCE_DIR}/volk.c
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${LUA51_SOURCES}
  ${bc7enc_SOURCE_DIR}/bc7enc.cpp)
# the platform layer headers (SDL3/, dlfcn.h, imgui_impl_sdl3.h) come first, nxvk's vulkan/ headers over the Khronos ones
target_include_directories(pt_thirdparty PUBLIC
  ${PT_ROOT}/src/platform/switch
  ${PT_NXVK_DIR}
  ${vkheaders_SOURCE_DIR}/include
  ${volk_SOURCE_DIR}
  ${vma_SOURCE_DIR}/include
  ${glm_SOURCE_DIR}
  ${imgui_SOURCE_DIR}
  ${lua51_SOURCE_DIR}/src
  ${stb_SOURCE_DIR}
  ${bc7enc_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/ogg-config
  /opt/devkitpro/portlibs/switch/include/harfbuzz
  /opt/devkitpro/portlibs/switch/include/freetype2
  # last: imgui_impl_sdl3.h resolves to the platform layer's replacement above, imgui_impl_vulkan.h to the real backend
  ${imgui_SOURCE_DIR}/backends)
# no LUA_USE_POSIX: it asks for _setjmp, which newlib does not have
target_compile_definitions(pt_thirdparty PUBLIC VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK VK_USE_PLATFORM_VI_NN IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(pt_thirdparty PRIVATE -w)

# Vorbis for the Wwise audio: the codebook library the desktop build downloads (hash checked), the sources compiled directly
set(PT_WWISE_CODEBOOKS_DIR ${CMAKE_BINARY_DIR}/_deps/ww2ogg)
set(PT_WWISE_CODEBOOKS_BIN ${PT_WWISE_CODEBOOKS_DIR}/packed_codebooks_aoTuV_603.bin)
set(PT_WWISE_CODEBOOKS_C ${PT_WWISE_CODEBOOKS_DIR}/wwise_codebooks.c)
if(NOT EXISTS ${PT_WWISE_CODEBOOKS_BIN})
  file(DOWNLOAD https://raw.githubusercontent.com/hcs64/ww2ogg/master/packed_codebooks_aoTuV_603.bin ${PT_WWISE_CODEBOOKS_BIN}
    EXPECTED_HASH SHA256=00a93eab267d281401b1efd54e888a2e183299b9e6c446c48d09f701a89d9d27 STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE ${PT_WWISE_CODEBOOKS_BIN})
    message(FATAL_ERROR "cannot download the Wwise Vorbis codebook library: ${status}")
  endif()
endif()
if(NOT EXISTS ${PT_WWISE_CODEBOOKS_C} OR ${PT_WWISE_CODEBOOKS_BIN} IS_NEWER_THAN ${PT_WWISE_CODEBOOKS_C})
  file(READ ${PT_WWISE_CODEBOOKS_BIN} hex HEX)
  string(LENGTH "${hex}" hex_length)
  math(EXPR size "${hex_length} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  file(WRITE ${PT_WWISE_CODEBOOKS_C}
    "#include <stddef.h>\nconst unsigned char pt_wwise_codebooks[] = {${bytes}};\nconst size_t pt_wwise_codebooks_size = ${size};\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/generated/ogg-config/ogg/config_types.h"
  "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
set(VORBIS_SOURCES)
foreach(name mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook
    sharedbook lookup bitrate)
  list(APPEND VORBIS_SOURCES ${vorbis_SOURCE_DIR}/lib/${name}.c)
endforeach()
add_library(pt_vorbis STATIC ${ogg_SOURCE_DIR}/src/bitwise.c ${ogg_SOURCE_DIR}/src/framing.c ${VORBIS_SOURCES} ${PT_WWISE_CODEBOOKS_C})
target_include_directories(pt_vorbis PUBLIC ${ogg_SOURCE_DIR}/include ${vorbis_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/ogg-config
  PRIVATE ${vorbis_SOURCE_DIR}/lib)
target_compile_options(pt_vorbis PRIVATE -w)

# libexpat: nxvk's driconf parsing needs it and devkitPro ships no build of it
set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${expat_SOURCE_DIR}/expat ${CMAKE_BINARY_DIR}/_deps/expat-build EXCLUDE_FROM_ALL)
# newlib declares arc4random_buf only under _GNU_SOURCE (C++ has it by default)
target_compile_definitions(expat PRIVATE _GNU_SOURCE)

# ---- generated headers (as ../CMakeLists.txt makes them) ----------------------------------------------------------------------

set(PT_VERSION "1.0.0")
set(PT_UPDATE_MANIFEST_URL "https://github.com/LoreanXavier/pt-pc/releases/latest/download/latest.json")
execute_process(COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/generated/pt_version.h" "-DPT_VERSION=${PT_VERSION}"
  "-DPT_UPDATE_MANIFEST_URL=${PT_UPDATE_MANIFEST_URL}" -P "${PT_ROOT}/cmake/version.cmake")
execute_process(COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/generated/build_id.h" "-DSOURCE=${PT_ROOT}" -P "${PT_ROOT}/cmake/build_id.cmake")

# ---- the engine ---------------------------------------------------------------------------------------------------------------

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${PT_ROOT}/src/engine/*.cpp)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS ${PT_ROOT}/src/game/*.cpp)
# the desktop only parts are replaced by src/platform/switch/switch_stubs.cpp: vendor upscalers and frame generation, Streamline,
# OpenXR, voice recognition, virtual pads, HTTP, LiveSplit, child processes
set(EXCLUDED
  engine/xr/xr_host.cpp
  engine/voice/microphone.cpp engine/voice/voice_recognizer.cpp
  engine/platform/http.cpp engine/platform/os.cpp engine/platform/virtual_pad.cpp engine/platform/livesplit.cpp
  engine/render/upscale/dlss_backend.cpp engine/render/upscale/frame_generation.cpp engine/render/upscale/fsr_backend.cpp
  engine/render/upscale/streamline.cpp engine/render/upscale/xess_backend.cpp engine/render/upscale/upscale_platform.cpp)
set(SOURCES ${ENGINE_SOURCES} ${GAME_SOURCES})
foreach(path ${EXCLUDED})
  list(REMOVE_ITEM SOURCES ${PT_ROOT}/src/${path})
endforeach()
list(APPEND SOURCES
  ${PT_ROOT}/src/main.cpp
  ${PT_ROOT}/src/platform/switch/sdl_switch.cpp
  ${PT_ROOT}/src/platform/switch/switch_stubs.cpp
  ${PT_ROOT}/src/platform/switch/lsfg_bridge.cpp
  ${PT_NXVK_SRC}/switch/smoke/nvk_compat.c)

# LSFG frame generation (GPL-3.0-or-later, third_party/lsfg-vk): its shaders are read from the user's Lossless.dll at run time
file(GLOB_RECURSE LSFG_SOURCES CONFIGURE_DEPENDS ${PT_ROOT}/third_party/lsfg-vk/lsfg-vk-*/src/*.cpp)
add_library(pt_lsfg STATIC ${LSFG_SOURCES})
target_include_directories(pt_lsfg PUBLIC ${PT_ROOT}/third_party/lsfg-vk/lsfg-vk-backend/include ${PT_ROOT}/third_party/lsfg-vk/lsfg-vk-common/include
  PRIVATE ${PT_ROOT}/third_party/lsfg-vk/lsfg-vk-backend/src ${PT_ROOT}/third_party/lsfg-vk/lsfg-vk-common/src)
target_link_libraries(pt_lsfg PUBLIC pt_thirdparty)
target_compile_options(pt_lsfg PRIVATE -w -ffunction-sections -fdata-sections)

add_executable(pt ${SOURCES})
target_include_directories(pt PRIVATE ${PT_ROOT}/src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(pt PRIVATE NOMINMAX GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS
  PT_SHADER_DIR="romfs:/shaders" PT_FONT_DIR="romfs:/fonts" PT_VOICE_MODEL_DIR="sdmc:/switch/pt/voice")
if(PT_RELEASE_LOCKS)
  target_compile_definitions(pt PRIVATE PT_RELEASE_LOCKS=1)
endif()
target_compile_definitions(pt PRIVATE PT_GAMEPLUS=1)
target_compile_options(pt PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(pt PRIVATE pt_lsfg pt_thirdparty pt_vorbis)

# nxvk: link flags as the driver's README requires (whole archive, the entry point kept, gc of the rest)
set(NXVK_LIBS
  -L${PT_NXVK_DIR}/lib
  -Wl,--whole-archive -lnvk -Wl,--no-whole-archive
  -Wl,--start-group -lnvk_support -lz -Wl,--end-group
  -Wl,-u,vk_icdGetInstanceProcAddr -Wl,--gc-sections)
# expat after the driver archives that call it (static libraries resolve left to right)
target_link_libraries(pt PRIVATE ${NXVK_LIBS} expat -lharfbuzz -lfreetype -lpng16 -lbz2 -lz -lnx -lm -lstdc++ -pthread)
if(NOT EXISTS ${PT_NXVK_DIR}/lib/libnvk.a)
  message(WARNING "nxvk is not built yet (${PT_NXVK_DIR}/lib/libnvk.a missing): the objects compile, the link will fail until it is")
endif()

# ---- shaders (SPIR-V) ----------------------------------------------------------------------------------------------------------

find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
file(GLOB SHADERS CONFIGURE_DEPENDS ${PT_ROOT}/shaders/*.vert ${PT_ROOT}/shaders/*.frag ${PT_ROOT}/shaders/*.comp)
# shaders and fonts are built into the .nro (romfs:/), so it runs from nxlink or the menu without extra files; the game dump is the SD card's only part
set(ROMFS ${CMAKE_BINARY_DIR}/romfs)
set(SD_ROOT ${CMAKE_BINARY_DIR}/sdmc/switch/pt)
if(GLSLC)
  set(SPIRV)
  foreach(shader ${SHADERS})
    get_filename_component(name ${shader} NAME)
    set(out ${ROMFS}/shaders/${name}.spv)
    set(dep ${CMAKE_BINARY_DIR}/shader-deps/${name}.d)  # outside the romfs: the depfiles must not be packed into the .nro
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS}/shaders ${CMAKE_BINARY_DIR}/shader-deps
      COMMAND ${GLSLC} --target-env=vulkan1.3 -O -MD -MF ${dep} ${shader} -o ${out}
      DEPENDS ${shader} DEPFILE ${dep} VERBATIM)
    list(APPEND SPIRV ${out})
  endforeach()
  add_custom_target(pt_shaders ALL DEPENDS ${SPIRV})
else()
  message(WARNING "glslc not found (install the Vulkan SDK or set VULKAN_SDK): the shaders are not compiled, see switch/README.md")
endif()

# ---- .nro ---------------------------------------------------------------------------------------------------------------------

set(DEVKITPRO $ENV{DEVKITPRO})
# Nothing of the game goes into a public .nro: the icon is the port's own (switch/assets/icon.jpg, drawn by switch/tools/make_icon.py), and
# the start-up picture is read at run time from the player's own dump (sce_sys/pic1.png). A personal build may carry the PS4 title art:
# icon.jpg and cover.jpg in switch/assets/private/ (.gitignore keeps that folder out of the repository) go into pt.nro with the original
# title. pt-public.nro (target pt_public_nro) is packed from the same program without them, for releases.
set(PT_PRIVATE_ASSETS ${PT_ROOT}/switch/assets/private)
set(PT_PUBLIC_TITLE "P.T. (Switch port)")
set(PT_PUBLIC_AUTHOR "r4lix, after LoreanXavier's P.T. PC Port")
if(EXISTS ${PT_PRIVATE_ASSETS}/icon.jpg)
  set(PT_NRO_ICON ${PT_PRIVATE_ASSETS}/icon.jpg)
  set(PT_NRO_TITLE "P.T.")
  set(PT_NRO_AUTHOR "7780s Studio")
else()
  set(PT_NRO_ICON ${PT_ROOT}/switch/assets/icon.jpg)
  set(PT_NRO_TITLE ${PT_PUBLIC_TITLE})
  set(PT_NRO_AUTHOR ${PT_PUBLIC_AUTHOR})
endif()
if(EXISTS ${PT_PRIVATE_ASSETS}/cover.jpg)
  set(PT_COVER_COMMAND ${CMAKE_COMMAND} -E copy ${PT_PRIVATE_ASSETS}/cover.jpg ${ROMFS}/cover.jpg)
else()
  set(PT_COVER_COMMAND ${CMAKE_COMMAND} -E rm -f ${ROMFS}/cover.jpg)
endif()
add_custom_command(TARGET pt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS}/fonts ${ROMFS}/shaders ${SD_ROOT}/save
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PT_ROOT}/assets/fonts ${ROMFS}/fonts
  COMMAND ${PT_COVER_COMMAND}
  COMMAND ${DEVKITPRO}/tools/bin/nacptool --create ${PT_NRO_TITLE} ${PT_NRO_AUTHOR} "${PT_VERSION}" ${CMAKE_BINARY_DIR}/pt.nacp
  COMMAND ${DEVKITPRO}/tools/bin/elf2nro $<TARGET_FILE:pt> ${CMAKE_BINARY_DIR}/pt.nro --icon=${PT_NRO_ICON}
          --nacp=${CMAKE_BINARY_DIR}/pt.nacp --romfsdir=${ROMFS}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/pt.nro ${SD_ROOT}/../pt.nro
  VERBATIM)
add_custom_target(pt_public_nro
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/romfs-public
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROMFS} ${CMAKE_BINARY_DIR}/romfs-public
  COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/romfs-public/cover.jpg ${CMAKE_BINARY_DIR}/romfs-public/test_env.txt
  COMMAND ${DEVKITPRO}/tools/bin/nacptool --create ${PT_PUBLIC_TITLE} ${PT_PUBLIC_AUTHOR} "${PT_VERSION}" ${CMAKE_BINARY_DIR}/pt-public.nacp
  COMMAND ${DEVKITPRO}/tools/bin/elf2nro $<TARGET_FILE:pt> ${CMAKE_BINARY_DIR}/pt-public.nro --icon=${PT_ROOT}/switch/assets/icon.jpg
          --nacp=${CMAKE_BINARY_DIR}/pt-public.nacp --romfsdir=${CMAKE_BINARY_DIR}/romfs-public
  DEPENDS pt
  VERBATIM)
