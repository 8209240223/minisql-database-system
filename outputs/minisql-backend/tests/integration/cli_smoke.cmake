# Exercise the actual executable, DLL deployment and process exit codes.
execute_process(COMMAND "${PROGRAM}" --version RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0 OR NOT output MATCHES "0.1.0")
    message(FATAL_ERROR "Version command failed: ${code}: ${output} ${error}")
endif()
execute_process(COMMAND "${PROGRAM}" --config "${PROJECT_DIR}/config/default.json" --check-config
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0 OR NOT output MATCHES "Configuration valid")
    message(FATAL_ERROR "Configuration check failed: ${code}: ${output} ${error}")
endif()
execute_process(COMMAND "${PROGRAM}" --print-config --port 9090
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "Print configuration failed: ${error}")
endif()
string(JSON port GET "${output}" server port)
if(NOT port EQUAL 9090)
    message(FATAL_ERROR "CLI override not applied")
endif()
execute_process(COMMAND "${PROGRAM}" --check-config --page-size 3000
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(code EQUAL 0 OR NOT error MATCHES "ConfigurationError")
    message(FATAL_ERROR "Invalid page size was not rejected")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(run_root "${CMAKE_CURRENT_BINARY_DIR}/cli-smoke-${run_id}")
execute_process(COMMAND "${PROGRAM}" --no-console
    --data "${run_root}/data"
    --wal-dir "${run_root}/wal"
    --catalog-dir "${run_root}/catalog"
    --log-dir "${run_root}/logs"
    --execute "CREATE TABLE t(id INT); INSERT INTO t VALUES(7); SELECT * FROM t;"
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "SQL execution failed: ${code}: ${output} ${error}")
endif()
string(JSON succeeded GET "${output}" success)
string(JSON selected GET "${output}" results 2 rows 0 0)
if(NOT succeeded OR NOT selected EQUAL 7)
    message(FATAL_ERROR "SQL execution returned an unexpected result: ${output}")
endif()
