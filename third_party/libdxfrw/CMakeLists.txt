# Vendored libdxfrw (LibreCAD fork, from LibreCAD 2.2.0.2).
# Built as a static library; vendored code keeps its own (relaxed) warnings.
file(GLOB DXFRW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intern/*.cpp
)

add_library(dxfrw STATIC ${DXFRW_SOURCES})
target_include_directories(dxfrw SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(dxfrw PROPERTIES POSITION_INDEPENDENT_CODE ON)
