find_package(SQLite3 REQUIRED)

add_library(vikicore STATIC
    Version.cpp
    geom/Vec2d.cpp
    geom/Xform2d.cpp
    geom/GeomUtil.cpp
    doc/Entity.cpp
    doc/Entities.cpp
    doc/EntityFactory.cpp
    doc/Document.cpp
    doc/EntitiesEx.cpp
    render/HitTest.cpp
    snap/SnapEngine.cpp
    cmd/Input.cpp
    cmd/CommandProcessor.cpp
    cmd/Commands2D.cpp
    cmd/CommandsDraw2.cpp
    cmd/CommandsModify.cpp
    io/NativeStore.cpp
    script/ScriptRunner.cpp
    solid/OcctOps.cpp
)

if(VIKICAD_HAS_DXF)
    target_sources(vikicore PRIVATE io/DxfImporter.cpp)
endif()

target_include_directories(vikicore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Qt6::Gui is for font metrics / QPainterPath only — QtWidgets is forbidden here.
target_link_libraries(vikicore
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        viki_warnings
        SQLite::SQLite3
        # OCCT modeling toolkits (visualization stays out of core)
        TKernel TKMath TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKBRep TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset
)
target_include_directories(vikicore SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})

if(VIKICAD_HAS_DXF)
    target_link_libraries(vikicore PRIVATE dxfrw)
    target_compile_definitions(vikicore PUBLIC VIKICAD_HAS_DXF=1)
endif()
