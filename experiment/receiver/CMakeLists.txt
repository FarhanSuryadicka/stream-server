cmake_minimum_required(VERSION 3.16)
# C as well as CXX: the vendored WebRTC dependencies (Mbed TLS, libSRTP,
# libjuice, usrsctp) are C, and without a configured C compiler CMake fails with
# "required internal CMake variable not set".
project(ucv_receiver C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
set(ENABLE_STDCXX_SYNC ON CACHE BOOL "" FORCE)
set(SRT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/srt)
add_subdirectory(${SRT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/srt EXCLUDE_FROM_ALL)

# WebRTC: the SAME vendored libdatachannel and Mbed TLS the Android app links,
# built from one tree for both sides. Two copies could drift to different DTLS
# or SRTP versions, and a handshake mismatch would look like a network fault on
# the rig rather than a version skew.
set(UCV_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp)
list(APPEND CMAKE_MODULE_PATH ${UCV_CPP_DIR}/cmake)
include(${UCV_CPP_DIR}/cmake/mbedtls-vendored.cmake)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${UCV_CPP_DIR}/libdatachannel ${CMAKE_BINARY_DIR}/ldc EXCLUDE_FROM_ALL)

# Build the same vendored libjpeg-turbo sources on the host so the Android
# decoder can be smoke-tested without a phone. The upstream project registers
# its full test/install suite unconditionally, so list its baseline sources as
# the Android build already does. SIMD is intentionally off: this checks
# lifecycle, recovery and output, not host JPEG performance.
set(UCV_JPEG_DIR ${UCV_CPP_DIR}/libjpeg-turbo)
set(UCV_JPEG_GEN_DIR ${CMAKE_BINARY_DIR}/jpeg-config)
set(JPEG_LIB_VERSION 62)
set(VERSION "2.1.5")
set(LIBJPEG_TURBO_VERSION_NUMBER 2001005)
set(BITS_IN_JSAMPLE 8)
set(MEM_SRCDST_SUPPORTED 1)
set(BUILD "host-smoke")
set(INLINE "inline")
set(THREAD_LOCAL "__thread")
set(SIZE_T ${CMAKE_SIZEOF_VOID_P})
set(COPYRIGHT_YEAR "2023")
configure_file(${UCV_JPEG_DIR}/jconfig.h.in ${UCV_JPEG_GEN_DIR}/jconfig.h @ONLY)
configure_file(${UCV_JPEG_DIR}/jconfigint.h.in ${UCV_JPEG_GEN_DIR}/jconfigint.h @ONLY)
configure_file(${UCV_JPEG_DIR}/jversion.h.in ${UCV_JPEG_GEN_DIR}/jversion.h @ONLY)

add_library(ucv-jpeg-host STATIC
    ${UCV_JPEG_DIR}/jcapimin.c ${UCV_JPEG_DIR}/jcapistd.c
    ${UCV_JPEG_DIR}/jccoefct.c ${UCV_JPEG_DIR}/jccolor.c
    ${UCV_JPEG_DIR}/jcdctmgr.c ${UCV_JPEG_DIR}/jchuff.c
    ${UCV_JPEG_DIR}/jcicc.c ${UCV_JPEG_DIR}/jcinit.c
    ${UCV_JPEG_DIR}/jcmainct.c ${UCV_JPEG_DIR}/jcmarker.c
    ${UCV_JPEG_DIR}/jcmaster.c ${UCV_JPEG_DIR}/jcomapi.c
    ${UCV_JPEG_DIR}/jcparam.c ${UCV_JPEG_DIR}/jcphuff.c
    ${UCV_JPEG_DIR}/jcprepct.c ${UCV_JPEG_DIR}/jcsample.c
    ${UCV_JPEG_DIR}/jctrans.c ${UCV_JPEG_DIR}/jdapimin.c
    ${UCV_JPEG_DIR}/jdapistd.c ${UCV_JPEG_DIR}/jdatadst.c
    ${UCV_JPEG_DIR}/jdatasrc.c ${UCV_JPEG_DIR}/jdcoefct.c
    ${UCV_JPEG_DIR}/jdcolor.c ${UCV_JPEG_DIR}/jddctmgr.c
    ${UCV_JPEG_DIR}/jdhuff.c ${UCV_JPEG_DIR}/jdicc.c
    ${UCV_JPEG_DIR}/jdinput.c ${UCV_JPEG_DIR}/jdmainct.c
    ${UCV_JPEG_DIR}/jdmarker.c ${UCV_JPEG_DIR}/jdmaster.c
    ${UCV_JPEG_DIR}/jdmerge.c ${UCV_JPEG_DIR}/jdphuff.c
    ${UCV_JPEG_DIR}/jdpostct.c ${UCV_JPEG_DIR}/jdsample.c
    ${UCV_JPEG_DIR}/jdtrans.c ${UCV_JPEG_DIR}/jerror.c
    ${UCV_JPEG_DIR}/jfdctflt.c ${UCV_JPEG_DIR}/jfdctfst.c
    ${UCV_JPEG_DIR}/jfdctint.c ${UCV_JPEG_DIR}/jidctflt.c
    ${UCV_JPEG_DIR}/jidctfst.c ${UCV_JPEG_DIR}/jidctint.c
    ${UCV_JPEG_DIR}/jidctred.c ${UCV_JPEG_DIR}/jquant1.c
    ${UCV_JPEG_DIR}/jquant2.c ${UCV_JPEG_DIR}/jutils.c
    ${UCV_JPEG_DIR}/jmemmgr.c ${UCV_JPEG_DIR}/jmemnobs.c
    ${UCV_JPEG_DIR}/jsimd_none.c
)
target_include_directories(ucv-jpeg-host PUBLIC
    ${UCV_JPEG_DIR} ${UCV_JPEG_GEN_DIR})

if(NOT CMAKE_BUILD_TYPE)
  # Measurement code compiled -O0 would add its own latency to the numbers it
  # reports, so Release is the default rather than an opt-in.
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ucv-receiver
    src/main.cpp
    src/ucv_net.cpp
    src/ucv_log.cpp
    src/ucv_rtsp.cpp
)
target_include_directories(ucv-receiver PRIVATE
    ${SRT_SOURCE_DIR}/srtcore
    ${SRT_SOURCE_DIR}/common
    ${CMAKE_BINARY_DIR}/srt
    ${UCV_CPP_DIR}/libdatachannel/include
)
target_link_libraries(ucv-receiver PRIVATE srt_static datachannel-static)

# Verifies the measurement primitives: wire layout, CRC rejection, clock-offset
# recovery, percentiles, loss/reorder accounting, and jitter's independence
# from clock offset. Run it before trusting any measurement — a bug in these
# does not crash, it silently produces plausible wrong numbers.
add_executable(ucv-selftest
    src/selftest.cpp
    src/ucv_net.cpp
    src/ucv_rtsp.cpp
)
target_include_directories(ucv-selftest PRIVATE ${UCV_CPP_DIR})

# Phone stand-in that speaks the real wire protocol, so the whole measurement
# path can be validated on one machine. Its numbers describe the loopback
# path, never a protocol.
add_executable(ucv-mocksender src/mocksender.cpp)

# Phone stand-in for the WebRTC path, so that measurement path can be validated
# on one machine too. Same caveat as ucv-mocksender: its numbers describe
# loopback, never WebRTC.
add_executable(ucv-webrtc-mocksender src/webrtc_mocksender.cpp)
target_include_directories(ucv-webrtc-mocksender PRIVATE
    ${UCV_CPP_DIR}/libdatachannel/include)
target_link_libraries(ucv-webrtc-mocksender PRIVATE datachannel-static)

# Mirrors the JPEG -> NV12 conversion in the Android encoder path. A chroma
# ordering or stride bug there does not crash — it produces a stream that
# encodes and transmits perfectly and looks wrong, which gets blamed on the
# protocol under test. Cheaper to catch here than on the rig.
add_executable(ucv-nv12test src/nv12test.cpp)

# Compiles the Android decoder source itself with only the MediaCodec half
# disabled. The valid -> corrupt -> valid sequence catches regressions where a
# libjpeg longjmp leaves the persistent decoder unusable for the next frame.
add_executable(ucv-jpegtest
    src/jpegtest.cpp
    ${UCV_CPP_DIR}/ucv_encoder.c
)
target_compile_definitions(ucv-jpegtest PRIVATE UCV_JPEG_ONLY)
target_include_directories(ucv-jpegtest PRIVATE
    ${UCV_CPP_DIR}
)
target_link_libraries(ucv-jpegtest PRIVATE ucv-jpeg-host)

set(UCV_TARGETS ucv-receiver ucv-selftest ucv-mocksender ucv-nv12test
    ucv-jpegtest ucv-webrtc-mocksender)

foreach(t IN LISTS UCV_TARGETS)
  target_include_directories(${t} PRIVATE include)
  if(WIN32)
    target_link_libraries(${t} PRIVATE ws2_32)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(${t} PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra)
  endif()

  # Static-link the toolchain runtime on MinGW. Without this the binaries
  # need libstdc++/libgcc/libwinpthread DLLs on PATH and die with
  # 0xc0000135 anywhere else — including under ctest, and on the measurement
  # PC if it is not the machine that built them.
  if(MINGW)
    target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endforeach()

enable_testing()
add_test(NAME primitives COMMAND ucv-selftest)
add_test(NAME nv12 COMMAND ucv-nv12test)
add_test(NAME jpeg_reuse COMMAND ucv-jpegtest)
