# monitor-core — measurement logic, deliberately free of Qt.
#
# Keeping this a plain C++20 library (FUTURE-SERVER-ARCHITECTURE.md section 7)
# means the same code can later back a headless monitor-core daemon whose
# recording and device sessions survive a UI restart. It also means these tests
# run anywhere.

add_library(ucv-core STATIC
    src/metrics.cpp
    src/ndjson_reader.cpp
    src/receiver_process.cpp
)

target_include_directories(ucv-core PUBLIC include)

if(MSVC)
  target_compile_options(ucv-core PRIVATE /W4)
  target_compile_definitions(ucv-core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ucv-core PRIVATE -Wall -Wextra)
endif()

if(WIN32)
  # CreateProcess / GenerateConsoleCtrlEvent live in kernel32, which MinGW and
  # MSVC both link by default; nothing extra is needed here.
else()
  find_package(Threads REQUIRED)
  target_link_libraries(ucv-core PUBLIC Threads::Threads)
endif()

add_executable(ucv-core-test tests/parity_test.cpp)
target_link_libraries(ucv-core-test PRIVATE ucv-core)

if(MINGW)
  # Match the receiver's linking so the binaries run on a machine that does not
  # have the MinGW runtime DLLs on PATH.
  target_link_options(ucv-core-test PRIVATE -static -static-libgcc -static-libstdc++)
endif()
