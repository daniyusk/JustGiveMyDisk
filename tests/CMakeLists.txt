add_executable(jgmd_smoke_tests smoke_tests.cpp)
target_link_libraries(jgmd_smoke_tests PRIVATE JustGiveMyDisk::core)
jgmd_enable_warnings(jgmd_smoke_tests)

add_test(NAME smoke.utf COMMAND jgmd_smoke_tests utf)
add_test(NAME smoke.parser COMMAND jgmd_smoke_tests parser)
add_test(NAME smoke.database COMMAND jgmd_smoke_tests database)
