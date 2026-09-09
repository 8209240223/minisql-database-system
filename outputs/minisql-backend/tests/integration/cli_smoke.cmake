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
execute_process(COMMAND "${PROGRAM}" --execute "SELECT 1;"
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(code EQUAL 0 OR NOT error MATCHES "NotImplementedError")
    message(FATAL_ERROR "Unimplemented SQL was not rejected")
endif()
