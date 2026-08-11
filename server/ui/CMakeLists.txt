# monitor-ui — Qt 6 desktop front end.
#
# Qt is found, never vendored: it is a large external SDK, and
# FUTURE-SERVER-ARCHITECTURE.md section 6 warns against pulling it into the tree
# speculatively. If Qt is absent, configure the project with -DUCV_BUILD_UI=OFF
# and the core library and its tests still build.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(ucv-monitor
    src/main.cpp
    src/run_list_model.cpp
    src/run_list_model.hpp
)

qt_add_qml_module(ucv-monitor
    URI ucv.qml
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/MetricChart.qml
    RESOURCE_PREFIX /
)

target_link_libraries(ucv-monitor PRIVATE
    ucv-core
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

set_target_properties(ucv-monitor PROPERTIES
    MACOSX_BUNDLE ON
)

if(WIN32)
  # WIN32_EXECUTABLE would pull in Qt6EntryPoint, whose prebuilt object
  # references __imp___argc / __imp___argv. Those come from the MinGW runtime
  # Qt itself was built against; a newer GCC (16.x here) no longer exports them,
  # so linking fails with an undefined reference that has nothing to do with
  # this code. Using the ordinary main() entry point avoids that library
  # entirely and works across toolchains.
  #
  # -mwindows still suppresses the console window, which is the only thing
  # WIN32_EXECUTABLE was wanted for.
  if(NOT UCV_CONSOLE)
    target_link_options(ucv-monitor PRIVATE -mwindows)
  endif()
endif()

# Copy the Qt runtime next to the executable after every build.
#
# Windows resolves DLLs from the executable's own directory FIRST, and that is
# the only reliable way to win here: Git for Windows ships its own
# libstdc++-6.dll in a directory that is on almost every PATH, and if that one
# is found before Qt's, Qt6Core fails to load with
#
#   "The procedure entry point _ZNKSt25__codecvt_utf8_utf16... could not be
#    located in the dynamic link library ucv-monitor.exe"
#
# The application never reaches main(), and nothing in the build output hints at
# it. Deploying the DLLs locally makes the app runnable by double-click without
# anyone having to curate PATH.
if(WIN32)
  find_program(UCV_WINDEPLOYQT windeployqt
      HINTS "${CMAKE_PREFIX_PATH}/bin" "${Qt6_DIR}/../../../bin")
  if(UCV_WINDEPLOYQT)
    add_custom_command(TARGET ucv-monitor POST_BUILD
        COMMAND "${UCV_WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                "$<TARGET_FILE:ucv-monitor>"
        COMMENT "Deploying Qt runtime beside ucv-monitor.exe"
        VERBATIM)
  else()
    message(WARNING
        "windeployqt not found — ucv-monitor.exe will only run with Qt's bin/ "
        "on PATH, and may pick up a conflicting libstdc++ from elsewhere.")
  endif()
endif()

if(MINGW)
  # Build with the SAME MinGW that built the installed Qt.
  #
  # Qt's official binaries ship their own libstdc++-6.dll beside the Qt DLLs,
  # and Qt6Core itself imports from it. Mixing toolchains fails at STARTUP, not
  # at link time: the app shows "procedure entry point ... could not be located"
  # and never reaches main(), with nothing in the build log to hint at it.
  # Static-linking the runtime does not help either, because the missing symbol
  # is imported by Qt's DLLs rather than by this binary.
  #
  # Qt 6.8.1 was built with GCC 13.1.0; aqtinstall provides a matching
  # toolchain as "tools_mingw1310". Pass it explicitly:
  #
  #   -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
  #
  # A mismatch is worth catching here rather than as a runtime dialog.
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14.0)
    message(WARNING
        "Compiling with GCC ${CMAKE_CXX_COMPILER_VERSION}. Qt's prebuilt MinGW "
        "binaries are usually built with GCC 13.x; a newer compiler links fine "
        "but the application then fails to START with a missing entry point. "
        "Use the toolchain that matches your Qt, e.g. "
        "C:/Qt/Tools/mingw1310_64/bin/g++.exe")
  endif()
endif()

if(MSVC)
  target_compile_options(ucv-monitor PRIVATE /W4)
  target_compile_definitions(ucv-monitor PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ucv-monitor PRIVATE -Wall -Wextra)
endif()
