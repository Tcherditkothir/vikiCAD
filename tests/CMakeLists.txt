find_package(Catch2 3 REQUIRED)
find_package(PkgConfig REQUIRED)
# Independent decoder for the animated-WebP round-trip tests.
pkg_check_modules(WebPDemux REQUIRED IMPORTED_TARGET libwebpdemux)

add_executable(vikicad-tests
    test_version.cpp
    test_vec2d.cpp
    test_geom_util.cpp
    test_regions.cpp
    test_occt_smoke.cpp
    test_document.cpp
    test_transaction_scope.cpp
    test_commands.cpp
    test_store.cpp
    test_entities2.cpp
    test_gear.cpp
    test_m2_commands.cpp
    test_cmd_ergonomics.cpp
    test_rect_profile.cpp
    test_snap.cpp
    test_editops.cpp
    test_annotations.cpp
    test_m5.cpp
    test_m7_solids.cpp
    test_workplane_planes.cpp
    test_solidops_guards.cpp
    test_split_combine.cpp
    test_interference.cpp
    test_section.cpp
    test_pattern3d.cpp
    test_move3d_points.cpp
    test_sweep_loft.cpp
    test_stl_export.cpp
    test_stl_import.cpp
    test_step_color.cpp
    test_obj_export.cpp
    test_clipboard.cpp
    test_params.cpp
    test_sketch.cpp
    test_feature_tree.cpp
    test_feature_params.cpp
    test_standard_views.cpp
    test_drawing_view.cpp
    test_inspect.cpp
    test_describe.cpp
    test_featedit.cpp
    test_subshape_ops.cpp
    test_solid_metrics.cpp
    test_edgeops_mate_draft.cpp
    test_gerber.cpp
    test_gerber_export.cpp
    test_excellon.cpp
    test_excellon_export.cpp
    test_gerberkit.cpp
    test_cam_inspect.cpp
    test_cam_export.cpp
    test_layer_stack.cpp
    test_mindist.cpp
    test_eagle_import.cpp
    test_iges_import.cpp
    test_anim_chain.cpp
    test_anim_clip.cpp
    test_anim_fk.cpp
    test_anim_avatar.cpp
    test_webp_writer.cpp
    test_offscreen_render.cpp
)

# Committed golden files (synthetic Gerber fixtures, ...).
target_compile_definitions(vikicad-tests PRIVATE
    VIKICAD_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

if(VIKICAD_HAS_DXF)
    target_sources(vikicad-tests PRIVATE test_dxf_import.cpp test_dxf_roundtrip.cpp
                   test_dwg_export.cpp)
endif()

# GLB round-trip through the INDEPENDENT vendored reader (cgltf); the
# exporter itself builds without it.
if(VIKICAD_HAS_CGLTF)
    target_sources(vikicad-tests PRIVATE test_anim_glb.cpp)
    target_link_libraries(vikicad-tests PRIVATE cgltf)
endif()

target_link_libraries(vikicad-tests PRIVATE vikicore vikioffscreen
    Catch2::Catch2WithMain viki_warnings PkgConfig::WebPDemux)

include(Catch)
catch_discover_tests(vikicad-tests)
