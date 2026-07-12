find_package(SQLite3 REQUIRED)

add_library(vikicore STATIC
    Version.cpp
    geom/Vec2d.cpp
    geom/Xform2d.cpp
    geom/GeomUtil.cpp
    geom/Intersect.cpp
    geom/Regions.cpp
    geom/GearGeometry.cpp
    doc/Entity.cpp
    doc/Entities.cpp
    doc/EntityFactory.cpp
    doc/Document.cpp
    doc/EntitiesEx.cpp
    doc/Annotations.cpp
    doc/Block.cpp
    doc/ArrayEntity.cpp
    doc/StickyNote.cpp
    doc/ParamTable.cpp
    render/HitTest.cpp
    render/StandardViews.cpp
    render/DrawingProjection.cpp
    snap/SnapEngine.cpp
    cmd/Input.cpp
    cmd/CommandProcessor.cpp
    cmd/Commands2D.cpp
    cmd/CommandsDraw2.cpp
    cmd/CommandsModify.cpp
    cmd/CommandsEdit.cpp
    cmd/CommandsAnnotate.cpp
    cmd/CommandsBlock.cpp
    cmd/CommandsArrayNote.cpp
    cmd/CommandsLayout.cpp
    cmd/Commands3D.cpp
    cmd/Commands3DFinish.cpp
    cmd/CommandsMeasure.cpp
    cmd/CommandsGear.cpp
    cmd/CommandsAssembly.cpp
    cmd/CommandsPattern3D.cpp
    cmd/CommandsParam.cpp
    cmd/CommandsSketch.cpp
    cmd/CommandsInspect.cpp
    cmd/CommandsDescribe.cpp
    cmd/CommandsFeatEdit.cpp
    cmd/CommandsSubShapeOps.cpp
    edit/EditOps.cpp
    io/NativeStore.cpp
    io/QueryJson.cpp
    io/PdfPlotter.cpp
    script/ScriptRunner.cpp
    solid/OcctOps.cpp
    solid/SolidEntity.cpp
    solid/SolidOps.cpp
    solid/FeatureTree.cpp
    solid/FeatureParams.cpp
    solid/SubShape.cpp
    io/StepIo.cpp
    io/StlIo.cpp
    io/ObjIo.cpp
)

if(VIKICAD_HAS_DXF)
    target_sources(vikicore PRIVATE io/DxfImporter.cpp io/DxfExporter.cpp)
endif()

target_include_directories(vikicore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Qt6::Gui is for font metrics / QPainterPath only — QtWidgets is forbidden here.
target_link_libraries(vikicore
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PUBLIC
        # OCCT modeling toolkits — public because SolidEntity.h exposes
        # TopoDS_Shape to consumers (visualization stays out of core).
        TKernel TKMath TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKBRep TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset
        TKDESTEP TKXSBase TKShHealing
        TKMesh TKDESTL
    PRIVATE
        viki_warnings
        SQLite::SQLite3
)
target_include_directories(vikicore SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})

if(VIKICAD_HAS_DXF)
    target_link_libraries(vikicore PRIVATE dxfrw)
    target_compile_definitions(vikicore PUBLIC VIKICAD_HAS_DXF=1)
endif()
