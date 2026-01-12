cmake_minimum_required(VERSION 3.16)

project(process-watchdog
    VERSION 1.0.0
    DESCRIPTION "Linux process supervisor with restart policies, heartbeats and resource limits"
    LANGUAGES CXX)

option(PW_BUILD_TESTS "Build unit and integration tests" ON)
option(PW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PW_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PW_ENABLE_COVERAGE "Instrument for gcov/gcovr coverage reports" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "process-watchdog targets Linux (epoll, signalfd)")
endif()

# Shared compile options for every target in this project.
add_library(pw_options INTERFACE)
target_compile_options(pw_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
    -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
if(PW_WARNINGS_AS_ERRORS)
    target_compile_options(pw_options INTERFACE -Werror)
endif()
if(PW_ENABLE_SANITIZERS)
    target_compile_options(pw_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                -fno-sanitize-recover=all)
    target_link_options(pw_options INTERFACE -fsanitize=address,undefined)
endif()
if(PW_ENABLE_COVERAGE)
    target_compile_options(pw_options INTERFACE --coverage -O0 -g)
    target_link_options(pw_options INTERFACE --coverage)
endif()

configure_file(cmake/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/pw/version.hpp @ONLY)

add_library(pwcore STATIC
    src/config.cpp
    src/control.cpp
    src/heartbeat.cpp
    src/ini.cpp
    src/logger.cpp
    src/process.cpp
    src/restart.cpp
    src/rotating_log.cpp
    src/socket.cpp
    src/supervisor.cpp
    src/util.cpp)
target_include_directories(pwcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(pwcore PUBLIC pw_options)

add_executable(pwatchd apps/pwatchd.cpp)
target_link_libraries(pwatchd PRIVATE pwcore)

add_executable(pwctl apps/pwctl.cpp)
target_link_libraries(pwctl PRIVATE pwcore)

include(GNUInstallDirs)
install(TARGETS pwatchd pwctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(PW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
