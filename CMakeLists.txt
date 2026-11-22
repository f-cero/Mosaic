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
    core/src/ms_link.c core/src/ms_device.c)
target_include_directories(mosaic_core PUBLIC core/include third_party)
target_compile_options(mosaic_core PRIVATE -Wall -Wextra)
target_link_libraries(mosaic_core PUBLIC m)
if(NOT MSVC)
  target_compile_options(mosaic_core PRIVATE -Wno-unused-parameter)
endif()

add_executable(mosaic tools/main.c)
target_link_libraries(mosaic PRIVATE mosaic_core)

enable_testing()
add_executable(mosaic_tests core/tests/test_all.c)
target_link_libraries(mosaic_tests PRIVATE mosaic_core)
add_test(NAME mosaic_tests COMMAND mosaic_tests)

# Format-conformance suite (SPR2/SPR1, cart16 v2, cross-mount)
add_executable(mosaic_conformance core/tests/test_conformance.c)
target_link_libraries(mosaic_conformance PRIVATE mosaic_core)
add_test(NAME mosaic_conformance COMMAND mosaic_conformance)

# Studio (SDL2 GUI) — see studio/README.md
find_package(SDL2 QUIET)
if(SDL2_FOUND)
  add_executable(mosaic-studio studio/main.c)
  target_include_directories(mosaic-studio PRIVATE ${SDL2_INCLUDE_DIRS} studio third_party/font)
  target_link_libraries(mosaic-studio PRIVATE mosaic_core ${SDL2_LIBRARIES})
else()
  message(STATUS "SDL2 not found — building CLI only (install libsdl2-dev)")
endif()
