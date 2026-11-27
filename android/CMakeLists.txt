# NDK build for the MVID decoder (libmvid.so).
#   cmake -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 .
#   cmake --build build-android
cmake_minimum_required(VERSION 3.18)
project(mvid C)
set(CMAKE_C_STANDARD 99)
add_library(mvid SHARED
    ../codec/mvid_util.c
    ../codec/mvid_decode.c
    jni/mvid_jni.c)
target_include_directories(mvid PRIVATE ../codec)
