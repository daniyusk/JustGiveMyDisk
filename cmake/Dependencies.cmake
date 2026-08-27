include(FetchContent)
find_package(Threads REQUIRED)

# Keep third-party source inputs immutable. These commits correspond to:
# CLI11 v2.5.0, fmt 11.2.0, FTXUI v5.0.0, and SQLite 3.50.4.
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG 4160d259d961cd393fd8d67590a8c7d210207348)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_OS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 40626af88bd7df9a5fb80be7b25ac85b122d6c21)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG cdf28903a7781f97ba94d30b79c3a4b0c97ccce7)

FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(cli11 fmt ftxui sqlite_amalgamation)

add_library(jgmd_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
add_library(SQLite::SQLite3 ALIAS jgmd_sqlite3)
target_include_directories(jgmd_sqlite3 SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_link_libraries(jgmd_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
