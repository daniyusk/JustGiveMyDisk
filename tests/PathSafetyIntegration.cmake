if (NOT PROGRAM OR NOT TEST_ROOT)
    message(FATAL_ERROR "PROGRAM and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(SOURCE "${TEST_ROOT}/source.img")
set(DATABASE "${TEST_ROOT}/scan.db")
set(DESTINATION_FILE "${TEST_ROOT}/destination-file")
file(WRITE "${SOURCE}" "not-an-ntfs-image")
file(WRITE "${DATABASE}" "not-a-database")
file(WRITE "${DESTINATION_FILE}" "existing-file")

execute_process(
    COMMAND "${PROGRAM}" scan "${SOURCE}" --out "${SOURCE}"
    RESULT_VARIABLE SAME_RESULT
    ERROR_VARIABLE SAME_ERROR)
if (SAME_RESULT EQUAL 0 OR NOT SAME_ERROR MATCHES "path validation failed for database.*alias, symlink, or equivalent path")
    message(FATAL_ERROR "scan did not reject the source as its SQLite database: ${SAME_ERROR}")
endif()

execute_process(
    COMMAND "${PROGRAM}" recover "${SOURCE}" "${DATABASE}" --id 1 --dest "${DESTINATION_FILE}"
    RESULT_VARIABLE DEST_RESULT
    ERROR_VARIABLE DEST_ERROR)
if (DEST_RESULT EQUAL 0 OR NOT DEST_ERROR MATCHES "path validation failed for destination.*destination is a file")
    message(FATAL_ERROR "recover did not reject a file destination: ${DEST_ERROR}")
endif()

if (UNIX)
    set(SOURCE_ALIAS "${TEST_ROOT}/source-alias")
    file(CREATE_LINK "${SOURCE}" "${SOURCE_ALIAS}" SYMBOLIC)
    execute_process(
        COMMAND "${PROGRAM}" scan "${SOURCE}" --out "${SOURCE_ALIAS}"
        RESULT_VARIABLE ALIAS_RESULT
        ERROR_VARIABLE ALIAS_ERROR)
    if (ALIAS_RESULT EQUAL 0 OR NOT ALIAS_ERROR MATCHES "alias, symlink, or equivalent path")
        message(FATAL_ERROR "scan did not reject a source symlink alias: ${ALIAS_ERROR}")
    endif()
endif()
