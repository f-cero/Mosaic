cmake_minimum_required(VERSION 3.16)
project(mosaic C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(mosaic_core STATIC
    core/src/ms_util.c core/src/ms_render.c core/src/ms_audio.c
    core/src/ms_input.c core/src/ms_cart.c core/src/ms_vm.c
    core/src/ms_game.c core/src/ms_core.c core/src/ms_build.c
    core/src/ms_link.c core/src/ms_device.c core/src/ms_video.c
    codec/mvid_util.c codec/mvid_decode.c)
target_include_directories(mosaic_core PUBLIC core/include third_party codec)
target_compile_options(mosaic_core PRIVATE -Wall -Wextra)
if(NOT WIN32)
  target_link_libraries(mosaic_core PUBLIC m)
endif()
if(NOT MSVC)
  target_compile_options(mosaic_core PRIVATE -Wno-unused-parameter)
endif()

add_executable(mosaic tools/main.c tools/video_main.c codec/mvid_encode.c)
target_include_directories(mosaic PRIVATE third_party)
target_link_libraries(mosaic PRIVATE mosaic_core)

enable_testing()
add_executable(mosaic_tests core/tests/test_all.c)
target_link_libraries(mosaic_tests PRIVATE mosaic_core)
add_test(NAME mosaic_tests COMMAND mosaic_tests)

# Format-conformance suite (SPR2/SPR1, cart16 v2, cross-mount)
add_executable(mosaic_conformance core/tests/test_conformance.c)
target_link_libraries(mosaic_conformance PRIVATE mosaic_core)
add_test(NAME mosaic_conformance COMMAND mosaic_conformance)

# MVID codec: lossless roundtrip, pan frames, cart packaging
add_executable(mosaic_video_test core/tests/test_video.c codec/mvid_encode.c)
target_include_directories(mosaic_video_test PRIVATE third_party codec)
target_link_libraries(mosaic_video_test PRIVATE mosaic_core)
add_test(NAME mosaic_video_test COMMAND mosaic_video_test)

# Studio (SDL2 GUI) — see studio/README.md
# SDL2 can come from a config package (Linux/SDL2_DIR) or from an explicit
# tree (-DSDL2_ROOT=/mingw64 on MSYS2/Windows), which avoids depending on a
# CMake config package that mingw packaging does not always ship.
set(MOSAIC_HAVE_SDL2 FALSE)
if(SDL2_ROOT)
  set(SDL2_INCLUDE_DIRS "${SDL2_ROOT}/include/SDL2")
  set(SDL2_LIBRARIES "-L${SDL2_ROOT}/lib;-lmingw32;-lSDL2main;-lSDL2")
  set(MOSAIC_HAVE_SDL2 TRUE)
  message(STATUS "Studio: SDL2 from SDL2_ROOT=${SDL2_ROOT}")
else()
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    set(MOSAIC_HAVE_SDL2 TRUE)
    message(STATUS "Studio: SDL2 from package config")
  endif()
endif()

if(MOSAIC_HAVE_SDL2)
  add_executable(mvidplay tools/mvidplay/main.c codec/mvid_util.c codec/mvid_decode.c)
  target_include_directories(mvidplay PRIVATE codec ${SDL2_INCLUDE_DIRS})
  target_link_libraries(mvidplay PRIVATE ${SDL2_LIBRARIES})

  add_executable(mosaic-studio studio/main.c)
  target_include_directories(mosaic-studio PRIVATE ${SDL2_INCLUDE_DIRS} studio third_party/font)
  target_link_libraries(mosaic-studio PRIVATE mosaic_core ${SDL2_LIBRARIES})
else()
  message(STATUS "SDL2 not found — building CLI only (install libsdl2-dev / pass -DSDL2_ROOT=)")
endif()
