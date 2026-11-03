# Cross toolchain for Luckfox Pico / RV1103
# Point LUCKFOX_SDK at the unpacked Luckfox SDK (contains the uClibc GCC).
#   cmake -DCMAKE_TOOLCHAIN_FILE=toolchain-luckfox.cmake -DLUCKFOX_SDK=/opt/luckfox-pico ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED LUCKFOX_SDK AND DEFINED ENV{LUCKFOX_SDK})
    set(LUCKFOX_SDK "$ENV{LUCKFOX_SDK}")
endif()

if(LUCKFOX_SDK)
    set(_LFX_TRIPLE "arm-rockchip830-linux-uclibcgnueabihf")
    set(_LFX_BIN "${LUCKFOX_SDK}/tools/linux/toolchain/${_LFX_TRIPLE}/bin")
    set(CMAKE_C_COMPILER   "${_LFX_BIN}/${_LFX_TRIPLE}-gcc")
    set(CMAKE_CXX_COMPILER "${_LFX_BIN}/${_LFX_TRIPLE}-g++")
    set(CMAKE_FIND_ROOT_PATH "${LUCKFOX_SDK}/tools/linux/toolchain/${_LFX_TRIPLE}/${_LFX_TRIPLE}/sysroot")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
