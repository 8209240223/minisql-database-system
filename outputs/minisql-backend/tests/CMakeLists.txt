find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(minisql_tests
    unit/error_test.cpp
    unit/config_test.cpp
    unit/logger_test.cpp
    integration/application_test.cpp)
target_link_libraries(minisql_tests PRIVATE minisql_server GTest::gtest_main Threads::Threads)
target_include_directories(minisql_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
include(GoogleTest)
gtest_discover_tests(minisql_tests DISCOVERY_TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME cli.process_smoke COMMAND "${CMAKE_COMMAND}"
    "-DPROGRAM=$<TARGET_FILE:minisql>"
    "-DPROJECT_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_smoke.cmake")
