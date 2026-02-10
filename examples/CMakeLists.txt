add_library(ehs_panel_core STATIC
    device_panel/api.cpp
    device_panel/device.cpp)
target_include_directories(ehs_panel_core PUBLIC device_panel)
target_link_libraries(ehs_panel_core PUBLIC ehs PRIVATE ehs_options)

add_executable(ehs-panel device_panel/main.cpp)
target_link_libraries(ehs-panel PRIVATE ehs_panel_core ehs_options)
target_compile_definitions(ehs-panel PRIVATE
    EHS_DEFAULT_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/device_panel/web")

add_executable(ehs-hello hello/main.cpp)
target_link_libraries(ehs-hello PRIVATE ehs ehs_options)

install(TARGETS ehs-panel ehs-hello RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY device_panel/web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/ehs/web)
