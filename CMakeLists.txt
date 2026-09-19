## VitaSDR — build for the PS Vita (VPK) or as a native test binary.
##
## Native core tests (no VitaSDK needed):
##   cmake -B build-native -DCMAKE_BUILD_TYPE=Release
##   cmake --build build-native
##   ./build-native/vitasdr_test
##
## Vita VPK (requires VitaSDK on PATH / $VITASDK set):
##   cmake -B build-vita \
##     -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
##     -DCMAKE_BUILD_TYPE=Release
##   cmake --build build-vita
##   # produces VitaSDR.vpk

cmake_minimum_required(VERSION 3.10)

## Auto-pick the VitaSDK toolchain when no toolchain was given and $VITASDK
## exists. Comment this block out to force a native build with VitaSDK present.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK}
   AND NOT DEFINED VITASDR_NATIVE)
  set(CMAKE_TOOLCHAIN_FILE
      "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()

project(VitaSDR C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

## Portable core: compiles with plain gcc and with arm-vita-eabi-gcc alike.
set(CORE_SOURCES
    src/core/adpcm.c
    src/core/jitter.c
    src/core/ws_client.c
    src/core/kiwi.c)

if(VITA)
  ## ---------------- Vita VPK build ----------------
  include("${VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "VitaSDR")
  set(VITA_TITLEID  "VSDR00001")
  set(VITA_VERSION  "01.00")

  add_compile_options(-Wall -Wextra -O2 -fno-lto)

  add_executable(${PROJECT_NAME}
      ${CORE_SOURCES}
      src/vita/net.c
      src/vita/audio.c
      src/vita/waterfall.c
      src/vita/ui.c
      src/vita/input.c
      src/vita/config.c
      src/vita/main.c)

  target_include_directories(${PROJECT_NAME} PRIVATE src/core)

  target_link_libraries(${PROJECT_NAME}
      vita2d
      SceNet_stub
      SceNetCtl_stub
      SceAppMgr_stub
      SceAppUtil_stub
      SceAudio_stub
      SceCtrl_stub
      SceTouch_stub
      SceDisplay_stub
      SceGxm_stub
      SceCommonDialog_stub
      SceSysmodule_stub
      ScePower_stub
      ScePgf_stub
      SceIofilemgr_stub
      SceRtc_stub
      freetype
      png
      jpeg
      z
      m)

  vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME})

else()
  ## ---------------- Native test build ----------------
  add_compile_options(-Wall -Wextra -D_DEFAULT_SOURCE)

  add_executable(vitasdr_test
      ${CORE_SOURCES}
      src/core/net_posix.c
      test/test_core.c)

  target_include_directories(vitasdr_test PRIVATE src/core)

  find_package(Threads REQUIRED)
  target_link_libraries(vitasdr_test Threads::Threads)

  enable_testing()
  add_test(NAME core COMMAND vitasdr_test)
endif()
