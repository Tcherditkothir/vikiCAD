find_package(Catch2 3 REQUIRED)

add_executable(vikicad-tests
    test_version.cpp
    test_vec2d.cpp
    test_occt_smoke.cpp
)

target_link_libraries(vikicad-tests PRIVATE vikicore Catch2::Catch2WithMain viki_warnings)

include(Catch)
catch_discover_tests(vikicad-tests)
