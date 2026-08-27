cmake_minimum_required(VERSION 3.20)

project(JustGiveMyDisk
    VERSION 0.1.0
    DESCRIPTION "Read-only NTFS MFT scanner for targeted directory recovery"
    LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/PathSafety.cpp
    src/RecoveryOutput.cpp
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

if (BUILD_TESTING)
    add_executable(path_safety_tests
        tests/PathSafetyTests.cpp
        src/PathSafety.cpp)
    target_include_directories(path_safety_tests PRIVATE src)
    target_link_libraries(path_safety_tests PRIVATE fmt::fmt)
    add_test(NAME path_safety_unit COMMAND path_safety_tests)

    add_executable(recovery_output_tests
        tests/RecoveryOutputTests.cpp
        src/RecoveryOutput.cpp)
    target_include_directories(recovery_output_tests PRIVATE src)
    target_link_libraries(recovery_output_tests PRIVATE fmt::fmt)
    add_test(NAME recovery_output_unit COMMAND recovery_output_tests)

    add_test(
        NAME path_safety_cli_integration
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:JustGiveMyDisk>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/path-safety-integration
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/PathSafetyIntegration.cmake)

    add_test(
        NAME recovery_output_cli_integration
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:JustGiveMyDisk>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/recovery-output-integration
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/RecoveryOutputIntegration.cmake)
endif()
