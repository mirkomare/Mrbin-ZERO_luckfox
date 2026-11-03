cmake_minimum_required(VERSION 3.16)
project(mrbin-core-luckfox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(HOST_SIM "Build without RKMPI / CSI (syntax-check on host)" ON)
option(LUCKFOX_RKMPI "Link Rockchip MPI (Luckfox SDK sysroot only)" OFF)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MRBIN_VERSION_FILE)
string(REGEX MATCH "^[^\n]+" MRBIN_VERSION "${MRBIN_VERSION_FILE}")
string(STRIP "${MRBIN_VERSION}" MRBIN_VERSION)
message(STATUS "MrBin CORE Luckfox ${MRBIN_VERSION}")

set(CORE_SOURCES
    src/main.cpp
    src/mrbin_core_main.cpp
    src/CoreAesUpload.cpp
    src/CoreAlarmSchedule.cpp
    src/CoreAuth.cpp
    src/CoreClipQueue.cpp
    src/CoreCrypto.cpp
    src/CoreDemo.cpp
    src/CoreEncFs.cpp
    src/CoreGPIO.cpp
    src/CoreH264Buffer.cpp
    src/CoreIspCal.cpp
    src/CoreLive.cpp
    src/CoreMrbinId.cpp
    src/CoreRecorder.cpp
    src/CoreRtsp.cpp
    src/CoreRtc.cpp
    src/CoreSD.cpp
    src/CoreSettings.cpp
    src/CoreSidecar.cpp
    src/CoreStatusLed.cpp
    src/CoreTime.cpp
    src/CoreVideo.cpp
    src/CoreVideoSensor.cpp
    src/CoreWeb.cpp
)

set(HAL_SOURCES
    platform/hal_common.cpp
    platform/hal_nvs.cpp
    platform/hal_gpio.cpp
    platform/hal_thread.cpp
    platform/hal_i2c.cpp
    platform/hal_httpd.cpp
    platform/hal_http_client.cpp
    platform/hal_net.cpp
    platform/hal_crypto.cpp
    platform/hal_muxer.cpp
    platform/hal_capture.cpp
    platform/hal_cjson.cpp
    platform/hal_sd.cpp
    platform/hal_misc.cpp
)

add_executable(mrbin-core ${CORE_SOURCES} ${HAL_SOURCES})

target_include_directories(mrbin-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
)

if(WIN32)
    target_include_directories(mrbin-core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/win
    )
    target_compile_definitions(mrbin-core PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(mrbin-core PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(mrbin-core PRIVATE
            /FI"${CMAKE_CURRENT_SOURCE_DIR}/platform/win/posix_compat.h")
    else()
        target_compile_options(mrbin-core PRIVATE
            -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/win/posix_compat.h")
    endif()
endif()

target_compile_definitions(mrbin-core PRIVATE
    MRBIN_VERSION="${MRBIN_VERSION}"
)

if(HOST_SIM)
    target_compile_definitions(mrbin-core PRIVATE HOST_SIM=1)
    message(STATUS "HOST_SIM=ON — GPIO mock, no CSI / RKMPI")
endif()

if(LUCKFOX_RKMPI)
    target_compile_definitions(mrbin-core PRIVATE LUCKFOX_RKMPI=1)
    if(NOT LUCKFOX_SDK AND DEFINED ENV{LUCKFOX_SDK})
        set(LUCKFOX_SDK "$ENV{LUCKFOX_SDK}")
    endif()
    message(STATUS "LUCKFOX_RKMPI=ON — Rockchip MPI from ${LUCKFOX_SDK}")
    if(LUCKFOX_SDK)
        set(_LFX_RK_INC "${LUCKFOX_SDK}/media/rockit/rockit/mpi/sdk/include")
        set(_LFX_RK_LIB "${LUCKFOX_SDK}/media/rockit/rockit/lib/lib32")
        set(_LFX_MPP_LIB "${LUCKFOX_SDK}/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf/lib")
        set(_LFX_RGA_LIB "${LUCKFOX_SDK}/media/rga/release_rga_rv1106_arm-rockchip830-linux-uclibcgnueabihf/lib")
        target_include_directories(mrbin-core PRIVATE
            "${LUCKFOX_SDK}/media/out/include"
            "${_LFX_RK_INC}"
        )
        target_link_libraries(mrbin-core PRIVATE
            "${_LFX_RK_LIB}/librockit.so"
            "${_LFX_MPP_LIB}/librockchip_mpp.so"
            "${_LFX_RGA_LIB}/librga.so"
            pthread dl m
        )
        target_link_options(mrbin-core PRIVATE "-Wl,-rpath,/usr/lib")
    endif()
endif()

find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_compile_definitions(mrbin-core PRIVATE HAVE_OPENSSL=1)
    target_link_libraries(mrbin-core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "OpenSSL: AES-128-CTR / AES-256-GCM / SHA-256 / HMAC / X25519")
else()
    message(WARNING "OpenSSL not found — crypto HAL returns errors (HOST_SIM syntax only)")
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
    target_compile_definitions(mrbin-core PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(mrbin-core PRIVATE CURL::libcurl)
    message(STATUS "libcurl: enroll + upload-aes/v1")
else()
    message(WARNING "libcurl not found — HTTPS enroll/upload disabled")
endif()

if(UNIX AND NOT HOST_SIM)
    find_package(Threads REQUIRED)
    target_link_libraries(mrbin-core PRIVATE Threads::Threads)
elseif(UNIX)
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(mrbin-core PRIVATE Threads::Threads)
    endif()
endif()

if(NOT MSVC)
    target_compile_options(mrbin-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS mrbin-core RUNTIME DESTINATION usr/bin)
install(FILES overlay/etc/init.d/S00mrbin-fast overlay/etc/init.d/S01mrbin-rec
              overlay/etc/init.d/S20mrbin-norkipc overlay/etc/init.d/S98mrbin-isp
              overlay/etc/init.d/S99mrbin
        DESTINATION etc/init.d
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(FILES overlay/usr/sbin/mrbin-boot-mode overlay/usr/sbin/mrbin-mount-sd
              overlay/usr/sbin/mrbin-isp overlay/usr/sbin/mrbin-insmod-cam.sh
              overlay/usr/sbin/mrbin-pir-launch overlay/usr/sbin/mrbin-pir-once
              DESTINATION usr/sbin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(FILES overlay/etc/mrbin/pins.env overlay/etc/mrbin/isp.env overlay/etc/mrbin/RkLunch.stub DESTINATION etc/mrbin)
