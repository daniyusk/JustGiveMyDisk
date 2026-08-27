cmake_minimum_required(VERSION 3.20)

project(JustGiveMyDisk
    VERSION 0.1.0
    DESCRIPTION "Read-only NTFS MFT scanner for targeted directory recovery"
    LANGUAGES C CXX)

include(CTest)

option(JUSTGIVEMYDISK_BUILD_APP "Build the JustGiveMyDisk command-line application" ON)
option(JUSTGIVEMYDISK_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JUSTGIVEMYDISK_BUILD_FUZZER "Build the libFuzzer target for MftRecordParser" OFF)

function(justgivemydisk_enable_sanitizers target)
    if (NOT JUSTGIVEMYDISK_ENABLE_SANITIZERS)
        return()
    endif()
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "JUSTGIVEMYDISK_ENABLE_SANITIZERS requires GCC or Clang")
    endif()

    target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(JGMD_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

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
    src/PathSafety.cpp
    src/RecoveryOutput.cpp
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
justgivemydisk_enable_sanitizers(jgmd_core)

if (JUSTGIVEMYDISK_BUILD_APP)
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
    justgivemydisk_enable_sanitizers(jgmd_tui)

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
    justgivemydisk_enable_sanitizers(JustGiveMyDisk)
endif()

if (BUILD_TESTING)
    add_subdirectory(tests)

    add_executable(MftRecordParserTests
        tests/MftRecordParserTests.cpp
        tests/NtfsFixtures.cpp
        src/MftRecord.cpp
        src/Utf.cpp)
    target_include_directories(MftRecordParserTests PRIVATE src tests)

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(MftRecordParserTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    justgivemydisk_enable_sanitizers(MftRecordParserTests)

    add_test(NAME MftRecordParserTests COMMAND MftRecordParserTests)

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

    if (JUSTGIVEMYDISK_BUILD_APP)
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
endif()

if (JUSTGIVEMYDISK_BUILD_FUZZER)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "JUSTGIVEMYDISK_BUILD_FUZZER requires Clang with libFuzzer")
    endif()

    add_executable(MftRecordParserFuzz
        tests/fuzz/MftRecordParserFuzz.cpp
        tests/NtfsFixtures.cpp
        src/MftRecord.cpp
        src/Utf.cpp)
    target_include_directories(MftRecordParserFuzz PRIVATE src tests)
    target_compile_options(MftRecordParserFuzz PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined)
    target_link_options(MftRecordParserFuzz PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()
