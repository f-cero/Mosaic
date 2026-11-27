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
    core/src/ms_dump.c
    core/src/ms_game.c core/src/ms_core.c core/src/ms_build.c
    core/src/ms_link.c core/src/ms_device.c core/src/ms_video.c
    core/src/ms_save.c
    codec/mvid_util.c codec/mvid_decode.c)
target_include_directories(mosaic_core PUBLIC core/include third_party codec)
target_compile_options(mosaic_core PRIVATE -Wall -Wextra)
if(NOT WIN32)
  target_link_libraries(mosaic_core PUBLIC m)
endif()
if(NOT MSVC)
  target_compile_options(mosaic_core PRIVATE -Wno-unused-parameter)
endif()

# `mosaic video web` inlines web/mvid.js so a clip exports as one HTML file.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/web_mvid_js.h
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/web/mvid.js
          -DOUT=${CMAKE_BINARY_DIR}/web_mvid_js.h
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/web/mvid.js ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
  COMMENT "Embedding web/mvid.js into the CLI")

add_executable(mosaic tools/main.c tools/video_main.c tools/video_web.c tools/audio_ingest.c
               codec/mvid_encode.c ${CMAKE_BINARY_DIR}/web_mvid_js.h)
target_include_directories(mosaic PRIVATE third_party ${CMAKE_BINARY_DIR})
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

add_executable(mosaic_matrix_test core/tests/test_matrix.c)
target_link_libraries(mosaic_matrix_test PRIVATE mosaic_core)
add_test(NAME mosaic_matrix_test COMMAND mosaic_matrix_test)

# Save image: A/B fallback, torn writes, cart binding, bundle, SAVE opcodes
add_executable(mosaic_save_test core/tests/test_save.c)
target_link_libraries(mosaic_save_test PRIVATE mosaic_core)
add_test(NAME mosaic_save_test COMMAND mosaic_save_test)
add_executable(mosaic_dump_test core/tests/test_dump.c)
target_link_libraries(mosaic_dump_test PRIVATE mosaic_core)
add_test(NAME mosaic_dump_test COMMAND mosaic_dump_test)

# The web decoder must render every frame bit-identically to the C decoder.
find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE AND EXISTS ${CMAKE_SOURCE_DIR}/tools/audio_test.py)
  add_test(NAME mosaic_audio_test
           COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/audio_test.py --mosaic $<TARGET_FILE:mosaic>)
endif()

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE AND EXISTS ${CMAKE_SOURCE_DIR}/web/mvid.js)
  add_test(NAME mosaic_web_codec_test
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mvid_web_test.mjs
                   --mosaic $<TARGET_FILE:mosaic>
                   --clip ${CMAKE_SOURCE_DIR}/examples/hello/assets/splash.mvid
           WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

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
