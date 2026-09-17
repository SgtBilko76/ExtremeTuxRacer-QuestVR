cmake_minimum_required(VERSION 3.22.1)
project(vrprobe C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../deps)

# Prebuilt Khronos OpenXR loader (arm64-v8a only), as vendored from the AAR.
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION ${DEPS_DIR}/openxr/jni/arm64-v8a/libopenxr_loader.so)

# NDK-provided glue for android.app.NativeActivity.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(vrprobe SHARED main.cpp)

target_include_directories(vrprobe PRIVATE ${DEPS_DIR}/openxr/include)

# ANativeActivity_onCreate lives in the glue and is only reached via dlsym,
# so the linker must be told not to drop it.
target_link_options(vrprobe PRIVATE "-u" "ANativeActivity_onCreate")

target_link_libraries(vrprobe
    native_app_glue
    openxr_loader
    EGL
    GLESv3
    android
    log)
