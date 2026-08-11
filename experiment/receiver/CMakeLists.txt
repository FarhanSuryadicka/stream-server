cmake_minimum_required(VERSION 3.16)
project(ucv_receiver CXX)

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

if(NOT CMAKE_BUILD_TYPE)
  # Measurement code compiled -O0 would add its own latency to the numbers it
  # reports, so Release is the default rather than an opt-in.
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ucv-receiver
    src/main.cpp
    src/ucv_net.cpp
    src/ucv_log.cpp
)
target_include_directories(ucv-receiver PRIVATE
    ${SRT_SOURCE_DIR}/srtcore
    ${SRT_SOURCE_DIR}/common
    ${CMAKE_BINARY_DIR}/srt
)
target_link_libraries(ucv-receiver PRIVATE srt_static)

# Verifies the measurement primitives: wire layout, CRC rejection, clock-offset
# recovery, percentiles, loss/reorder accounting, and jitter's independence
# from clock offset. Run it before trusting any measurement — a bug in these
# does not crash, it silently produces plausible wrong numbers.
add_executable(ucv-selftest src/selftest.cpp)

# Phone stand-in that speaks the real wire protocol, so the whole measurement
# path can be validated on one machine. Its numbers describe the loopback
# path, never a protocol.
add_executable(ucv-mocksender src/mocksender.cpp)

# Mirrors the JPEG -> NV12 conversion in the Android encoder path. A chroma
# ordering or stride bug there does not crash — it produces a stream that
# encodes and transmits perfectly and looks wrong, which gets blamed on the
# protocol under test. Cheaper to catch here than on the rig.
add_executable(ucv-nv12test src/nv12test.cpp)

set(UCV_TARGETS ucv-receiver ucv-selftest ucv-mocksender ucv-nv12test)

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
