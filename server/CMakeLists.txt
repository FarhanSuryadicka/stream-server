cmake_minimum_required(VERSION 3.21)
project(ucv_monitor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The UI is optional so monitor-core and its tests build on a machine with no
# Qt installed. That is not a convenience: it is what keeps the measurement
# logic testable in CI and on the rig PC without dragging in a 2 GB SDK.
option(UCV_BUILD_UI "Build the Qt 6 desktop UI" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(core)

if(UCV_BUILD_UI)
  add_subdirectory(ui)
else()
  message(STATUS "ucv-monitor: UI disabled (UCV_BUILD_UI=OFF); core only")
endif()

enable_testing()
add_test(NAME core_parity COMMAND ucv-core-test)
