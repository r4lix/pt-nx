# Toolchain file for the Nintendo Switch port (devkitA64 + libnx).
# Use: cmake -G Ninja -B build/switch -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake
if(NOT DEFINED ENV{DEVKITPRO})
  set(ENV{DEVKITPRO} "/opt/devkitpro")
endif()
set(DEVKITPRO "$ENV{DEVKITPRO}")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(PT_SWITCH ON CACHE BOOL "Building for Nintendo Switch")

set(CMAKE_C_COMPILER   "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++")
set(CMAKE_AR           "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-ar")
set(CMAKE_RANLIB       "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-ranlib")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PT_SWITCH_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE")
set(CMAKE_C_FLAGS_INIT   "${PT_SWITCH_ARCH} -D__SWITCH__ -DPT_SWITCH=1 -ffunction-sections")
set(CMAKE_CXX_FLAGS_INIT "${PT_SWITCH_ARCH} -D__SWITCH__ -DPT_SWITCH=1 -ffunction-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs ${PT_SWITCH_ARCH}")

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/portlibs/switch" "${DEVKITPRO}/libnx")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
include_directories(SYSTEM "${DEVKITPRO}/libnx/include" "${DEVKITPRO}/portlibs/switch/include")
link_directories("${DEVKITPRO}/libnx/lib" "${DEVKITPRO}/portlibs/switch/lib")
