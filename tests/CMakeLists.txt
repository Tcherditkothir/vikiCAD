find_package(Catch2 3 REQUIRED)

add_executable(vikicad-tests
    test_version.cpp
    test_vec2d.cpp
    test_geom_util.cpp
    test_regions.cpp
    test_occt_smoke.cpp
    test_document.cpp
    test_commands.cpp
    test_store.cpp
    test_entities2.cpp
    test_gear.cpp
    test_m2_commands.cpp
    test_snap.cpp
    test_editops.cpp
    test_annotations.cpp
    test_m5.cpp
    test_m7_solids.cpp
    test_split_combine.cpp
    test_interference.cpp
    test_section.cpp
    test_pattern3d.cpp
    test_sweep_loft.cpp
    test_stl_export.cpp
    test_obj_export.cpp
    test_params.cpp
    test_feature_tree.cpp
    test_feature_params.cpp
    test_standard_views.cpp
    test_drawing_view.cpp
)

if(VIKICAD_HAS_DXF)
    target_sources(vikicad-tests PRIVATE test_dxf_import.cpp test_dxf_roundtrip.cpp)
endif()

target_link_libraries(vikicad-tests PRIVATE vikicore Catch2::Catch2WithMain viki_warnings)

include(Catch)
catch_discover_tests(vikicad-tests)
