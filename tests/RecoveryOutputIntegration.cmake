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

# 1. Verify that CLI recover documents --overwrite flag
execute_process(
    COMMAND "${PROGRAM}" recover --help
    RESULT_VARIABLE HELP_RESULT
    OUTPUT_VARIABLE HELP_OUTPUT
    ERROR_VARIABLE HELP_ERROR)
if (NOT HELP_RESULT EQUAL 0 OR NOT HELP_OUTPUT MATCHES "--overwrite")
    message(FATAL_ERROR "recover CLI does not expose --overwrite flag: ${HELP_OUTPUT} ${HELP_ERROR}")
endif()

# 2. Verify that recover rejects destination if it is an existing file
execute_process(
    COMMAND "${PROGRAM}" recover "${SOURCE}" "${DATABASE}" --id 1 --dest "${DESTINATION_FILE}"
    RESULT_VARIABLE FILE_DEST_RESULT
    ERROR_VARIABLE FILE_DEST_ERROR)
if (FILE_DEST_RESULT EQUAL 0 OR NOT FILE_DEST_ERROR MATCHES "path validation failed for destination.*destination is a file")
    message(FATAL_ERROR "recover did not reject a file destination: ${FILE_DEST_ERROR}")
endif()

# 3. Verify that path safety rejects destination alias or symlink pointing to source
if (UNIX)
    set(DEST_SOURCE_ALIAS "${TEST_ROOT}/dest-source-alias")
    file(CREATE_LINK "${SOURCE}" "${DEST_SOURCE_ALIAS}" SYMBOLIC)
    execute_process(
        COMMAND "${PROGRAM}" recover "${SOURCE}" "${DATABASE}" --id 1 --dest "${DEST_SOURCE_ALIAS}"
        RESULT_VARIABLE ALIAS_RESULT
        ERROR_VARIABLE ALIAS_ERROR)
    if (ALIAS_RESULT EQUAL 0 OR NOT ALIAS_ERROR MATCHES "alias, symlink, or equivalent path")
        message(FATAL_ERROR "recover did not reject destination source alias: ${ALIAS_ERROR}")
    endif()
endif()
