cmake_minimum_required(VERSION 3.20)

project(JustGiveMyDisk
    VERSION 0.1.0
    DESCRIPTION "Read-only NTFS MFT scanner for targeted directory recovery"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(JGMD_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

include(CTest)
include(cmake/Dependencies.cmake)

function(jgmd_enable_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if (JGMD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
            -Wundef)
        if (JGMD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(jgmd_core
    src/Scanner.cpp
    src/Recover.cpp
    src/MftRecord.cpp
    src/Database.cpp
    src/Utf.cpp)
add_library(JustGiveMyDisk::core ALIAS jgmd_core)

target_include_directories(jgmd_core PUBLIC src)
target_include_directories(jgmd_core SYSTEM PRIVATE "${fmt_SOURCE_DIR}/include")
target_link_libraries(jgmd_core
    PUBLIC SQLite::SQLite3
    PRIVATE fmt::fmt)
jgmd_enable_warnings(jgmd_core)

add_library(jgmd_tui src/Tui.cpp)
target_include_directories(jgmd_tui SYSTEM PRIVATE
    "${fmt_SOURCE_DIR}/include"
    "${ftxui_SOURCE_DIR}/include")
target_link_libraries(jgmd_tui
    PUBLIC JustGiveMyDisk::core
    PRIVATE
        fmt::fmt
        ftxui::screen
        ftxui::dom
        ftxui::component)
jgmd_enable_warnings(jgmd_tui)

add_executable(JustGiveMyDisk src/main.cpp)
target_include_directories(JustGiveMyDisk SYSTEM PRIVATE
    "${cli11_SOURCE_DIR}/include"
    "${fmt_SOURCE_DIR}/include")
target_link_libraries(JustGiveMyDisk
    PRIVATE
        JustGiveMyDisk::core
        jgmd_tui
        CLI11::CLI11
        fmt::fmt)
jgmd_enable_warnings(JustGiveMyDisk)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()
