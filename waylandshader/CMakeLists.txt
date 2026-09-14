cmake_minimum_required(VERSION 3.25)
project(WaylandShaderNiri VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_path(LIBRASHADER_INCLUDE_DIR librashader.h REQUIRED)
find_library(LIBRASHADER_LIBRARY waylandshader-rashader REQUIRED)

add_library(waylandshader_niri_bridge STATIC bridge.cpp)
set_target_properties(waylandshader_niri_bridge PROPERTIES
    OUTPUT_NAME waylandshader-niri-bridge
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(waylandshader_niri_bridge PUBLIC . PRIVATE "${LIBRASHADER_INCLUDE_DIR}")
target_link_libraries(waylandshader_niri_bridge PRIVATE PkgConfig::EPOXY "${LIBRASHADER_LIBRARY}")

add_library(niri_controller_client STATIC client/controller_client.cpp)
target_include_directories(niri_controller_client PUBLIC client)
target_link_libraries(niri_controller_client PUBLIC Qt6::Core Qt6::DBus)
add_executable(waylandshader-niri-controller client/controller_main.cpp)
target_link_libraries(waylandshader-niri-controller PRIVATE niri_controller_client Qt6::Widgets)
add_executable(waylandshader-nirictl client/ctl_main.cpp)
target_link_libraries(waylandshader-nirictl PRIVATE niri_controller_client)


option(WAYLANDSHADER_BUILD_TESTS "Build real GPU EGLImage regressions" OFF)
if(WAYLANDSHADER_BUILD_TESTS)
    enable_testing()
    add_executable(niri_bridge_test tests/niri_bridge_test.cpp)
    target_link_libraries(niri_bridge_test PRIVATE waylandshader_niri_bridge Qt6::Core Qt6::Gui PkgConfig::EPOXY)
    add_test(NAME niri_bridge COMMAND niri_bridge_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set_tests_properties(niri_bridge PROPERTIES SKIP_RETURN_CODE 77)
endif()
install(TARGETS waylandshader-niri-controller waylandshader-nirictl
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS "${NIRI_EXECUTABLE}" DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME niri-waylandshader)
install(FILES "${LIBRASHADER_LIBRARY}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/waylandshader")
install(FILES org.waylandshader.NiriController.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES niri-waylandshader.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/wayland-sessions")
install(FILES
    ../docs/waylandshader/README.md
    ../docs/waylandshader/HISTORY.md
    ../docs/waylandshader/UPGRADING.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/niri-waylandshader")
install(FILES ../LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/niri-waylandshader" RENAME niri-GPL-3.0)
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/niri-waylandshader" RENAME waylandshader-MIT)
install(FILES "${RASHADER_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/niri-waylandshader" RENAME librashader-MPL-2.0)
