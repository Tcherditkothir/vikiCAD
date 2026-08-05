# Offscreen 3D rendering for the animation CLI (and its tests). Kept OUT
# of vikicore on purpose: core stays visualization-free (see the toolkit
# comment in core/CMakeLists.txt) — only targets that actually render link
# the OCCT visualization toolkits.
add_library(vikioffscreen STATIC
    OffscreenRenderer.cpp
)
target_include_directories(vikioffscreen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vikioffscreen
    PUBLIC
        vikicore
        TKV3d TKService TKOpenGl
    PRIVATE
        viki_warnings
)
