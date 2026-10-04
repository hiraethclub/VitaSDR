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
    src/core/kiwi.c
    src/core/bandplan.c
    src/core/resamp.c
    src/core/httpget.c
    src/core/kiwidir.c
    src/core/owrx.c
    src/core/tls.c
    src/core/ca_bundle.c)

## Vendored BearSSL (TLS for OpenWebRX over wss://). Built as a static library
## so the linker pulls in only the object files actually referenced. Third-party
## code, so warnings are silenced (-w) to keep our own build warning-clean.
file(GLOB_RECURSE BEARSSL_SOURCES CONFIGURE_DEPENDS third_party/bearssl/src/*.c)
add_library(bearssl STATIC ${BEARSSL_SOURCES})
target_include_directories(bearssl
    PUBLIC  third_party/bearssl/inc
    PRIVATE third_party/bearssl/src)
target_compile_options(bearssl PRIVATE -w -O2)

if(VITA)
  ## ---------------- Vita VPK build ----------------
  include("${VITASDK}/share/vita.cmake" REQUIRED)

  ## Build revision from git (commit count + short hash), used in the app name,
  ## log filename and VPK filename so builds are tellable apart.
  execute_process(COMMAND git rev-list --count HEAD
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND git rev-parse --short HEAD
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT GIT_COUNT)
    set(GIT_COUNT 0)
  endif()
  if(NOT GIT_HASH)
    set(GIT_HASH "nogit")
  endif()
  set(BUILD_REV "${GIT_COUNT}-g${GIT_HASH}")

  ## VITASDR_DEV builds a SEPARATE app (distinct TITLEID, name, data dir, VPK
  ## name) so it installs alongside the stable VitaSDR without clashing.
  ## -DVITASDR_DEV=OFF produces the stable identity.
  option(VITASDR_DEV "Build the separate dev app" ON)
  if(VITASDR_DEV)
    set(VITA_APP_NAME "VitaSDR Dev r${GIT_COUNT}")
    set(VITA_TITLEID  "VSDR00002")
    set(APP_DATA_DIR  "ux0:data/vitasdr-dev")
    set(VPK_FILE      "VitaSDR-dev-r${BUILD_REV}.vpk")
  else()
    set(VITA_APP_NAME "VitaSDR")
    set(VITA_TITLEID  "VSDR00001")
    set(APP_DATA_DIR  "ux0:data/vitasdr")
    set(VPK_FILE      "VitaSDR.vpk")
  endif()
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
      src/vita/log.c
      src/vita/b64.c
      src/vita/font.c
      src/vita/menu.c
      src/vita/main.c)

  target_include_directories(${PROJECT_NAME} PRIVATE src/core)

  target_compile_definitions(${PROJECT_NAME} PRIVATE
      VITASDR_DATA_DIR="${APP_DATA_DIR}"
      VITASDR_BUILD_REV="${BUILD_REV}"
      VITASDR_APP_LABEL="${VITA_APP_NAME}")

  target_link_libraries(${PROJECT_NAME}
      bearssl
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
      bz2
      m)

  vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

  ## Custom LiveArea assets (icon bubble + background + startup image). An
  ## earlier build shipped a malformed icon0.png that VitaShell rejected
  ## (install error 0x80113D); these are freshly generated as valid 128x128
  ## RGBA / 840x500 / 280x158 non-interlaced PNGs. Toggle off to fall back to
  ## the default bubble: -DVITASDR_LIVEAREA=OFF
  option(VITASDR_LIVEAREA "Bundle custom icon/LiveArea assets" ON)
  if(VITASDR_LIVEAREA)
    vita_create_vpk(${VPK_FILE} ${VITA_TITLEID} ${PROJECT_NAME}.self
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME}
        FILE sce_sys/icon0.png sce_sys/icon0.png
        FILE sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
        FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
        FILE resources/font.ttf resources/font.ttf)
  else()
    vita_create_vpk(${VPK_FILE} ${VITA_TITLEID} ${PROJECT_NAME}.self
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME}
        FILE resources/font.ttf resources/font.ttf)
  endif()

else()
  ## ---------------- Native test build ----------------
  add_compile_options(-Wall -Wextra -D_DEFAULT_SOURCE)

  add_executable(vitasdr_test
      ${CORE_SOURCES}
      src/core/net_posix.c
      test/test_core.c)

  target_include_directories(vitasdr_test PRIVATE src/core)

  find_package(Threads REQUIRED)
  target_link_libraries(vitasdr_test bearssl Threads::Threads m)

  enable_testing()
  add_test(NAME core COMMAND vitasdr_test)
endif()
