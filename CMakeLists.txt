cmake_minimum_required(VERSION 3.20)

project(JustGiveMyDisk
    VERSION 0.1.0
    DESCRIPTION "Read-only NTFS MFT scanner for targeted directory recovery"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CLI11 REQUIRED)
find_package(fmt REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(JustGiveMyDisk
    src/main.cpp
    src/Scanner.cpp
    src/MftRecord.cpp
    src/Database.cpp
    src/Utf.cpp)

target_include_directories(JustGiveMyDisk PRIVATE src)

target_link_libraries(JustGiveMyDisk
    PRIVATE
        CLI11::CLI11
        fmt::fmt
        SQLite3::SQLite3)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(JustGiveMyDisk PRIVATE -Wall -Wextra -Wpedantic)
endif()
