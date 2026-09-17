# --------------------------------------------------------------------
# Extreme Tux Racer -- Android / Meta Quest build
#
# The upstream project builds with autotools (Linux) or an MSBuild project
# (Windows); neither can target Android, so this is a parallel build
# definition rather than a replacement. The source list below is kept in
# step with src/Makefile.am, which lists every .cpp explicitly.
#
# The desktop dev tools (ogl_test, tools, tool_char, tool_frame) are
# excluded: they exist to author character shapes and keyframes on a
# workstation and have no place in a headset build.
# --------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22.1)
project(etr CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(SRC  ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --------------------------------------------------------------------
#	Dependencies
# --------------------------------------------------------------------

# Quest immersive apps are NativeActivity-based and never receive a 2D
# window surface, which rules out SDL's Android backend: it will not start
# its main thread until a surface exists. The platform layer therefore owns
# EGL and the lifecycle itself, using the NDK's glue directly.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Prebuilt Khronos OpenXR loader, vendored from the official AAR.
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION ${DEPS}/openxr/jni/arm64-v8a/libopenxr_loader.so)

# --------------------------------------------------------------------
#	Game sources (mirrors src/Makefile.am)
# --------------------------------------------------------------------

set(GAME_SOURCES
    ${SRC}/audio.cpp
    ${SRC}/common.cpp
    ${SRC}/config_screen.cpp
    ${SRC}/course.cpp
    ${SRC}/course_render.cpp
    ${SRC}/credits.cpp
    ${SRC}/env.cpp
    ${SRC}/event.cpp
    ${SRC}/event_select.cpp
    ${SRC}/font.cpp
    ${SRC}/game_config.cpp
    ${SRC}/game_ctrl.cpp
    ${SRC}/game_over.cpp
    ${SRC}/game_type_select.cpp
    ${SRC}/gui.cpp
    ${SRC}/help.cpp
    ${SRC}/hud.cpp
    ${SRC}/intro.cpp
    ${SRC}/keyframe.cpp
    ${SRC}/loading.cpp
    ${SRC}/main.cpp
    ${SRC}/mathlib.cpp
    ${SRC}/matrices.cpp
    ${SRC}/newplayer.cpp
    ${SRC}/ogl.cpp
    ${SRC}/particles.cpp
    ${SRC}/paused.cpp
    ${SRC}/physics.cpp
    ${SRC}/quadtree.cpp
    ${SRC}/race_select.cpp
    ${SRC}/racing.cpp
    ${SRC}/regist.cpp
    ${SRC}/reset.cpp
    ${SRC}/score.cpp
    ${SRC}/splash_screen.cpp
    ${SRC}/spx.cpp
    ${SRC}/states.cpp
    ${SRC}/textures.cpp
    ${SRC}/track_marks.cpp
    ${SRC}/translation.cpp
    ${SRC}/tux.cpp
    ${SRC}/vectors.cpp
    ${SRC}/view.cpp
    ${SRC}/winsys.cpp
)

# GLES 3 shim standing in for the fixed-function pipeline.
set(GLES_SOURCES
    ${SRC}/gles/gl_compat.cpp
    ${SRC}/gles/gl_shaders.cpp
)

# SFML-compatible platform layer on the NDK + stb.
set(PLATFORM_SOURCES
    ${SRC}/platform/sfml_system.cpp
    ${SRC}/platform/sfml_window.cpp
    ${SRC}/platform/sfml_graphics.cpp
    ${SRC}/platform/sfml_audio.cpp
    ${SRC}/platform/stb_impl.c
    ${SRC}/platform/android_assets.cpp
    ${SRC}/platform/android_main.cpp
)

# OpenXR session, stereo frame driver and Touch controller input.
set(VR_SOURCES
    ${SRC}/vr/vr.cpp
    ${SRC}/vr/xr_manager.cpp
    ${SRC}/vr/xr_input.cpp
)

add_library(etr SHARED
    ${GAME_SOURCES}
    ${GLES_SOURCES}
    ${PLATFORM_SOURCES}
    ${VR_SOURCES}
)

target_include_directories(etr PRIVATE
    ${SRC}
    ${SRC}/platform      # makes <SFML/...> resolve to the shim
    ${DEPS}/stb
    ${DEPS}/openxr/include
)

target_compile_definitions(etr PRIVATE
    ETR_ANDROID=1
    # xr_manager.hpp defines XR_USE_PLATFORM_ANDROID and
    # XR_USE_GRAPHICS_API_OPENGL_ES itself before including <openxr.h>.
    ENABLE_OPENXR=1
)

# The upstream sources are from 2016 and trip a lot of modern warnings that
# are not worth churning the tree over; keep the ones that matter.
target_compile_options(etr PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-reorder-ctor
    -Wno-deprecated-declarations
)

# ANativeActivity_onCreate lives in the glue and is only reached via
# dlsym, so the linker has to be told to keep it.
target_link_options(etr PRIVATE "-u" "ANativeActivity_onCreate")

target_link_libraries(etr
    native_app_glue
    openxr_loader
    EGL
    GLESv3
    aaudio
    android
    log
)
