add_executable(vikicad
    main.cpp
    MainWindow.cpp
    PreferencesDialog.cpp
    Theme.cpp
    canvas/CanvasWidget.cpp
    panels/CommandBar.cpp
    panels/LayerPanel.cpp
    panels/PropertiesPanel.cpp
    panels/AssemblyPanel.cpp
    panels/ToolPanels.cpp
    ipc/RpcServer.cpp
    occview/OcctViewWidget.cpp
)

target_include_directories(vikicad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vikicad PRIVATE vikicore Qt6::Widgets Qt6::Network viki_warnings
    TKV3d TKService TKOpenGl)
