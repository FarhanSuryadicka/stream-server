cmake_minimum_required(VERSION 3.22)
project(uvcserver_native C CXX)

set(LIBUSB_INSTALL_TARGETS OFF)
set(LIBUSB_BUILD_SHARED_LIBS OFF)
set(LIBUSB_BUILD_EXAMPLES OFF)
set(LIBUSB_BUILD_TESTING OFF)
add_subdirectory(libusb-cmake)

# SRT v1.5.6, vendored from the official Haivision repository. The first
# comparison profile is deliberately unencrypted; encryption and its crypto
# backend are a separate variable and must not silently change transport cost.
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
set(ENABLE_STDCXX_SYNC ON CACHE BOOL "" FORCE)
add_subdirectory(srt EXCLUDE_FROM_ALL)

# Native JPEG codec for the dewarp pipeline (decode camera MJPEG -> RGB,
# encode dewarped RGB -> JPEG) — replaces the earlier Kotlin/Bitmap JNI
# round-trip so decode/encode run in C alongside the GLES render, no per-
# frame JNI array marshaling.
#
# libjpeg-turbo's own CMakeLists.txt deliberately refuses add_subdirectory()
# (hard FATAL_ERROR, upstream policy — "use ExternalProject_Add() instead"),
# so this lists its baseline (no SIMD, no arithmetic coding — matches the
# JPEG_SOURCES set in libjpeg-turbo's own CMakeLists.txt when those options
# are off) sources directly, the same way this project already vendors
# libuvc's sources without going through libuvc's own build system.
set(JPEG_TURBO_DIR libjpeg-turbo)
set(JPEG_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/jpeg-config)

set(JPEG_LIB_VERSION 62)
set(VERSION "2.1.5")
set(LIBJPEG_TURBO_VERSION_NUMBER 2001005)
set(BITS_IN_JSAMPLE 8)
set(MEM_SRCDST_SUPPORTED 1)
set(BUILD "20260805")
set(INLINE "inline")
set(THREAD_LOCAL "__thread")
set(SIZE_T 8)
set(HAVE_BUILTIN_CTZL 1)
set(COPYRIGHT_YEAR "2023")
# C_ARITH_CODING_SUPPORTED / D_ARITH_CODING_SUPPORTED / WITH_SIMD /
# RIGHT_SHIFT_IS_UNSIGNED / HAVE_INTRIN_H intentionally left unset so their
# #cmakedefine lines below come out commented-out (off).

# jconfigint.h.in's PACKAGE_NAME wants @CMAKE_PROJECT_NAME@ substituted to
# libjpeg-turbo's own name, but CMAKE_PROJECT_NAME is a real CMake builtin
# already set to this project's own name (uvcserver_native) — save/restore
# it so this cosmetic header string doesn't clobber it build-wide.
set(_SAVED_CMAKE_PROJECT_NAME ${CMAKE_PROJECT_NAME})
set(CMAKE_PROJECT_NAME "libjpeg-turbo")
configure_file(${JPEG_TURBO_DIR}/jconfig.h.in ${JPEG_GEN_DIR}/jconfig.h @ONLY)
configure_file(${JPEG_TURBO_DIR}/jconfigint.h.in ${JPEG_GEN_DIR}/jconfigint.h @ONLY)
configure_file(${JPEG_TURBO_DIR}/jversion.h.in ${JPEG_GEN_DIR}/jversion.h @ONLY)
set(CMAKE_PROJECT_NAME ${_SAVED_CMAKE_PROJECT_NAME})

# Neon SIMD for the JPEG codec — the dewarp pipeline's per-frame JPEG
# decode/encode at 3040x3040 is the CPU bottleneck, and NEON roughly halves
# it. This is the same source set upstream's simd/CMakeLists.txt builds for
# arm64 with NEON_INTRINSICS enabled (NDK clang has the full intrinsics, so
# the hybrid GAS path / jsimd_neon.S is not needed). neon-compat.h is
# configured like jconfig.h above; all three HAVE_* intrinsics exist in
# NDK clang, so they're all set.
set(NEON_SIMD_SOURCES
    ${JPEG_TURBO_DIR}/simd/arm/jccolor-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jcgray-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jcphuff-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jcsample-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jdcolor-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jdmerge-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jdsample-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jfdctfst-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jfdctint-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jidctfst-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jidctint-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jidctred-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/jquanti-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/aarch64/jchuff-neon.c
    ${JPEG_TURBO_DIR}/simd/arm/aarch64/jsimd.c
)
set(HAVE_VLD1_S16_X3 1)
set(HAVE_VLD1_U16_X2 1)
set(HAVE_VLD1Q_U8_X4 1)
configure_file(${JPEG_TURBO_DIR}/simd/arm/neon-compat.h.in ${JPEG_GEN_DIR}/neon-compat.h @ONLY)

add_library(jpeg-static STATIC
    ${JPEG_TURBO_DIR}/jcapimin.c
    ${JPEG_TURBO_DIR}/jcapistd.c
    ${JPEG_TURBO_DIR}/jccoefct.c
    ${JPEG_TURBO_DIR}/jccolor.c
    ${JPEG_TURBO_DIR}/jcdctmgr.c
    ${JPEG_TURBO_DIR}/jchuff.c
    ${JPEG_TURBO_DIR}/jcicc.c
    ${JPEG_TURBO_DIR}/jcinit.c
    ${JPEG_TURBO_DIR}/jcmainct.c
    ${JPEG_TURBO_DIR}/jcmarker.c
    ${JPEG_TURBO_DIR}/jcmaster.c
    ${JPEG_TURBO_DIR}/jcomapi.c
    ${JPEG_TURBO_DIR}/jcparam.c
    ${JPEG_TURBO_DIR}/jcphuff.c
    ${JPEG_TURBO_DIR}/jcprepct.c
    ${JPEG_TURBO_DIR}/jcsample.c
    ${JPEG_TURBO_DIR}/jctrans.c
    ${JPEG_TURBO_DIR}/jdapimin.c
    ${JPEG_TURBO_DIR}/jdapistd.c
    ${JPEG_TURBO_DIR}/jdatadst.c
    ${JPEG_TURBO_DIR}/jdatasrc.c
    ${JPEG_TURBO_DIR}/jdcoefct.c
    ${JPEG_TURBO_DIR}/jdcolor.c
    ${JPEG_TURBO_DIR}/jddctmgr.c
    ${JPEG_TURBO_DIR}/jdhuff.c
    ${JPEG_TURBO_DIR}/jdicc.c
    ${JPEG_TURBO_DIR}/jdinput.c
    ${JPEG_TURBO_DIR}/jdmainct.c
    ${JPEG_TURBO_DIR}/jdmarker.c
    ${JPEG_TURBO_DIR}/jdmaster.c
    ${JPEG_TURBO_DIR}/jdmerge.c
    ${JPEG_TURBO_DIR}/jdphuff.c
    ${JPEG_TURBO_DIR}/jdpostct.c
    ${JPEG_TURBO_DIR}/jdsample.c
    ${JPEG_TURBO_DIR}/jdtrans.c
    ${JPEG_TURBO_DIR}/jerror.c
    ${JPEG_TURBO_DIR}/jfdctflt.c
    ${JPEG_TURBO_DIR}/jfdctfst.c
    ${JPEG_TURBO_DIR}/jfdctint.c
    ${JPEG_TURBO_DIR}/jidctflt.c
    ${JPEG_TURBO_DIR}/jidctfst.c
    ${JPEG_TURBO_DIR}/jidctint.c
    ${JPEG_TURBO_DIR}/jidctred.c
    ${JPEG_TURBO_DIR}/jquant1.c
    ${JPEG_TURBO_DIR}/jquant2.c
    ${JPEG_TURBO_DIR}/jutils.c
    ${JPEG_TURBO_DIR}/jmemmgr.c
    ${JPEG_TURBO_DIR}/jmemnobs.c
    ${NEON_SIMD_SOURCES}
)
set_target_properties(jpeg-static PROPERTIES OUTPUT_NAME jpeg)
# WITH_SIMD is required for correctness with the NEON sources: it makes
# MULTIPLIER = short (the quant-table layout the Neon IDCT assumes) and
# switches jmemmgr to 32-byte-aligned pools + the SIMD divisor tables.
target_compile_definitions(jpeg-static PRIVATE WITH_SIMD NEON_INTRINSICS)
target_include_directories(jpeg-static
    PUBLIC
        ${JPEG_TURBO_DIR}
        ${JPEG_GEN_DIR}
)

set(LIBUVC_SOURCES
    libuvc/src/ctrl.c
    libuvc/src/ctrl-gen.c
    libuvc/src/device.c
    libuvc/src/diag.c
    libuvc/src/frame.c
    libuvc/src/init.c
    libuvc/src/misc.c
    libuvc/src/stream.c
)

set(libuvc_VERSION_MAJOR 0)
set(libuvc_VERSION_MINOR 0)
set(libuvc_VERSION_PATCH 7)
set(libuvc_VERSION 0.0.7)

configure_file(
    libuvc/include/libuvc/libuvc_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/libuvc/libuvc_config.h
    @ONLY
)

add_library(uvc STATIC ${LIBUVC_SOURCES})
target_include_directories(uvc
    PUBLIC
        libuvc/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)
target_link_libraries(uvc
    PRIVATE
        libusb::usb-1.0
)

add_library(uvcserver SHARED
    jni_bridge.c
    ucv_transport.c
    ucv_transport_mjpeg.c
    ucv_transport_rtp.c
    ucv_transport_rtsp.c
    ucv_transport_hls.c
    ucv_transport_srt.c
    ucv_control.c
    ucv_encoder.c
    ucv_pipeline.c
)
# The wire format header is shared verbatim with the PC receiver
# (experiment/receiver/include/ucv_wire.h) — one definition, two targets, so
# the two sides cannot silently drift apart.
target_include_directories(uvcserver
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../experiment/receiver/include
        ${CMAKE_CURRENT_SOURCE_DIR}/srt/srtcore
        ${CMAKE_CURRENT_SOURCE_DIR}/srt/common
        ${CMAKE_CURRENT_BINARY_DIR}/srt
)
target_link_libraries(uvcserver
    PRIVATE
        uvc
        libusb::usb-1.0
        log
        m
        jpeg-static
        srt_static
        # MediaCodec via the NDK, so capture -> decode -> encode -> transport
        # all run in native code on one clock (see ucv_encoder.h).
        mediandk
)
