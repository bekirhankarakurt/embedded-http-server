cmake_minimum_required(VERSION 3.16)

project(embedded_http_server
    VERSION 1.0.0
    DESCRIPTION "Zero-dependency HTTP/1.1 server for embedded Linux devices"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "embedded-http-server requires Linux (epoll, eventfd)")
endif()

option(EHS_BUILD_TESTS "Build unit and integration tests" ON)
option(EHS_BUILD_EXAMPLES "Build the device panel and hello examples" ON)
option(EHS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(EHS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(EHS_COVERAGE "Instrument for gcov/gcovr coverage" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ehs_options INTERFACE)
target_compile_options(ehs_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
if(EHS_WARNINGS_AS_ERRORS)
    target_compile_options(ehs_options INTERFACE -Werror)
endif()
if(EHS_SANITIZE)
    target_compile_options(ehs_options INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(ehs_options INTERFACE -fsanitize=address,undefined)
endif()
if(EHS_COVERAGE)
    target_compile_options(ehs_options INTERFACE --coverage -O0 -g)
    target_link_options(ehs_options INTERFACE --coverage)
endif()

configure_file(include/ehs/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/ehs/version.hpp @ONLY)

add_library(ehs
    src/auth.cpp
    src/http.cpp
    src/json.cpp
    src/parser.cpp
    src/router.cpp
    src/server.cpp
    src/session.cpp
    src/static_files.cpp)
add_library(ehs::ehs ALIAS ehs)
target_include_directories(ehs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>)
target_compile_features(ehs PUBLIC cxx_std_17)
target_link_libraries(ehs PRIVATE $<BUILD_INTERFACE:ehs_options>)

include(GNUInstallDirs)

if(EHS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(EHS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS ehs ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ehs DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/ehs/version.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ehs)
