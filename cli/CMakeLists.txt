add_executable(vikicad-cli
    main.cpp
)

target_link_libraries(vikicad-cli PRIVATE vikicore vikioffscreen Qt6::Network
    viki_warnings)
