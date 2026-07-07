cmake_minimum_required(VERSION 3.22)

project(VikiCAD
    VERSION 0.1.0
    DESCRIPTION "2D drafting / 3D solid CAD for Linux"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VIKICAD_BUILD_GUI   "Build the vikicad GUI application" ON)
option(VIKICAD_BUILD_CLI   "Build the vikicad-cli headless tool" ON)
option(VIKICAD_BUILD_TESTS "Build the test suite" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(VikiWarnings)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(OpenCASCADE REQUIRED)

set(CMAKE_AUTOMOC ON)

# libdxfrw is vendored; tolerate its absence until the sources land so the
# skeleton builds offline. DXF I/O is compiled out when missing.
set(VIKICAD_HAS_DXF OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdxfrw/src")
    add_subdirectory(third_party/libdxfrw)
    set(VIKICAD_HAS_DXF ON)
else()
    message(WARNING "third_party/libdxfrw/src not found — building without DXF support")
endif()

add_subdirectory(core)

if(VIKICAD_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_subdirectory(gui)
endif()

if(VIKICAD_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(VIKICAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
