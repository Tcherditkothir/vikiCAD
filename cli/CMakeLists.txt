add_executable(vikicad-cli
    main.cpp
)

target_link_libraries(vikicad-cli PRIVATE vikicore viki_warnings)
