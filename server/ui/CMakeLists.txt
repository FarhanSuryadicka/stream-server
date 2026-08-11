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
    WIN32_EXECUTABLE ON        # no console window behind the GUI
    MACOSX_BUNDLE ON
)

if(MSVC)
  target_compile_options(ucv-monitor PRIVATE /W4)
  target_compile_definitions(ucv-monitor PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ucv-monitor PRIVATE -Wall -Wextra)
endif()
