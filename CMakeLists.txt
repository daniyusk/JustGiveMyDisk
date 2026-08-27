cmake_minimum_required(VERSION 3.20)

project(JustGiveMyDisk
    VERSION 0.1.0
    DESCRIPTION "Read-only NTFS MFT scanner for targeted directory recovery"
    LANGUAGES CXX)

include(CTest)

option(JUSTGIVEMYDISK_BUILD_APP "Build the JustGiveMyDisk command-line application" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (JUSTGIVEMYDISK_BUILD_APP)
    find_package(CLI11 REQUIRED)
    find_package(fmt REQUIRED)
    find_package(SQLite3 REQUIRED)

    find_package(ftxui QUIET)
    if (NOT ftxui_FOUND)
        include(FetchContent)
        FetchContent_Declare(ftxui
            GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
            GIT_TAG v5.0.0)
        FetchContent_MakeAvailable(ftxui)
    endif()

    add_executable(JustGiveMyDisk
        src/main.cpp
        src/Scanner.cpp
        src/Recover.cpp
        src/Tui.cpp
        src/MftRecord.cpp
        src/Database.cpp
        src/Utf.cpp)

    target_include_directories(JustGiveMyDisk PRIVATE src)

    target_link_libraries(JustGiveMyDisk
        PRIVATE
            CLI11::CLI11
            fmt::fmt
            ftxui::screen
            ftxui::dom
            ftxui::component
            SQLite3::SQLite3)

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(JustGiveMyDisk PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if (BUILD_TESTING)
    add_executable(MftRecordParserTests
        tests/MftRecordParserTests.cpp
        tests/NtfsFixtures.cpp
        src/MftRecord.cpp
        src/Utf.cpp)
    target_include_directories(MftRecordParserTests PRIVATE src tests)

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(MftRecordParserTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME MftRecordParserTests COMMAND MftRecordParserTests)
endif()
