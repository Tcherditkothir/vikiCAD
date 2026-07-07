add_executable(vikicad
    main.cpp
    MainWindow.cpp
    canvas/CanvasWidget.cpp
    panels/CommandBar.cpp
    panels/LayerPanel.cpp
    panels/PropertiesPanel.cpp
)

target_include_directories(vikicad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vikicad PRIVATE vikicore Qt6::Widgets viki_warnings)
