add_library(vikicore STATIC
    Version.cpp
    geom/Vec2d.cpp
    doc/Document.cpp
    solid/OcctOps.cpp
)

target_include_directories(vikicore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Qt6::Gui is for font metrics / QPainterPath only — QtWidgets is forbidden here.
target_link_libraries(vikicore
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        viki_warnings
        # OCCT modeling toolkits (visualization stays out of core)
        TKernel TKMath TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKBRep TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset
)
target_include_directories(vikicore SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})

if(VIKICAD_HAS_DXF)
    target_link_libraries(vikicore PRIVATE dxfrw)
    target_compile_definitions(vikicore PRIVATE VIKICAD_HAS_DXF=1)
endif()
