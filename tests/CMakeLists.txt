add_executable(ehs_tests
    test_main.cpp
    test_fuzz.cpp
    test_json.cpp
    test_parser.cpp
    test_router.cpp
    test_server.cpp
    test_session.cpp
    test_static.cpp)
target_link_libraries(ehs_tests PRIVATE ehs ehs_options)
target_include_directories(ehs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

foreach(suite parser fuzz json router auth static_files session server)
    add_test(NAME ${suite} COMMAND ehs_tests ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 120)
endforeach()

if(EHS_BUILD_EXAMPLES)
    target_sources(ehs_tests PRIVATE test_panel.cpp)
    target_link_libraries(ehs_tests PRIVATE ehs_panel_core)
    add_test(NAME panel COMMAND ehs_tests panel)

    add_test(NAME panel_replay
        COMMAND ehs-panel --replay ${PROJECT_SOURCE_DIR}/examples/device_panel/replay/session.http --no-web)
    set_tests_properties(panel_replay PROPERTIES
        PASS_REGULAR_EXPRESSION "HTTP/1.1 202 Accepted.*\"hostname\":\"replayed-node\".*Connection: close")
    add_test(NAME panel_cli_help COMMAND ehs-panel --help)
    set_tests_properties(panel_cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: ehs-panel")
endif()
