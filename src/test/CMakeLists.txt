cmake_minimum_required(VERSION 3.5)
project(aiapi_test CXX)

# ------------------------------------------------------------------
# Support two usages:
#   1) standalone:  cmake -S src/test -B build/test_build
#   2) from root:   add_subdirectory(src/test)
# In standalone mode Drogon targets are not yet available, so resolve them here.
# ------------------------------------------------------------------
if (NOT TARGET Drogon::Drogon)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.deps/drogon-install")
        list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../.deps/drogon-install")
    endif()
    find_package(Drogon CONFIG REQUIRED)
endif()

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)   # RFC-001 P9: 必须与主程序一致,原为 20
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Standalone test builds do not include src/CMakeLists.txt, so provide the same
# opt-in coverage helper when it is not already defined by the parent build.
if (NOT COMMAND aiapi_enable_coverage)
    option(AIAPI_ENABLE_COVERAGE "enable gcov/llvm-cov instrumentation" OFF)
    function(aiapi_enable_coverage target_name)
        if (NOT AIAPI_ENABLE_COVERAGE)
            return()
        endif()
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
            set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS " --coverage")
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target_name} PRIVATE
                -O0 -g -fprofile-instr-generate -fcoverage-mapping)
            set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS
                " -fprofile-instr-generate -fcoverage-mapping")
        else()
            message(FATAL_ERROR
                "AIAPI_ENABLE_COVERAGE supports only GCC or Clang; got ${CMAKE_CXX_COMPILER_ID}")
        endif()
    endfunction()
endif()

enable_testing()


# 测试源文件
set(TEST_SOURCES
    test_main.cc
    test_error_event.cpp
    test_error_stats_config.cpp
    test_response_index.cpp
    test_continuity_resolver.cpp
    test_history_replay_budget.cpp
    test_outbound_budget.cpp
    test_request_adapters.cpp
    test_xml_tool_call_codec.cpp
    test_bridge_protocol_codec.cpp
    test_bridge_helpers.cpp
    test_action_protocol.cpp
    test_tool_call_validator.cpp
    test_strict_client_rules.cpp
    test_forced_tool_call.cpp
    test_normalize_tool_args.cpp
    test_sinks.cpp
    test_io_loop_response_stream.cpp
    test_generation_service_emit.cpp
    test_generation_service_bridge_fixture.cpp
    test_chayns_model_catalog.cpp
    test_chayns_message_correlation.cpp
    test_chayns_fixtures.cpp
    test_chayns_provider_fixture.cpp
    test_chayns_browser_impersonation.cpp
    test_login_response_log_summary.cpp
    test_background_task_queue_shutdown.cpp
    test_application_shutdown_harness.cpp
    test_shutdown_workers.cpp
    test_retool_workspace_store_port.cpp
    test_retool_provider_fixture.cpp
    test_provider_result_codec.cpp
    test_channel_store_port.cpp
    test_account_store_port.cpp
    test_account_lifecycle_fixture.cpp
    test_account_reload_ghost_index.cpp
    test_account_pool_heap_order.cpp
    test_retool_provision_health.cpp
    test_health_ready_probe.cpp
    test_controller_utils.cpp
    test_provider_retirement.cpp
    stub_db_collaborators.cpp
)

# P3-W1: tests no longer compile production .cpp files.  The canonical target
# is created by src/CMakeLists.txt; standalone configuration is intentionally
# rejected so source ownership cannot silently fork again.
if (NOT TARGET aiapi_legacy)
    message(FATAL_ERROR
        "Configure tests from the repository root: aiapi_test must link aiapi_legacy")
endif()

add_executable(${PROJECT_NAME} ${TEST_SOURCES})
aiapi_enable_coverage(${PROJECT_NAME})

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE aiapi_legacy aiapi_runtime Drogon::Drogon)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if (UNIX)
    add_executable(aiapi_shutdown_signal_fixture
        support/shutdown_signal_fixture.cpp)
    aiapi_enable_coverage(aiapi_shutdown_signal_fixture)
    target_link_libraries(aiapi_shutdown_signal_fixture PRIVATE
        aiapi_legacy aiapi_runtime Drogon::Drogon)
    add_dependencies(${PROJECT_NAME} aiapi_shutdown_signal_fixture)
endif()

if (COMMAND ParseAndAddDrogonTests)
    ParseAndAddDrogonTests(${PROJECT_NAME})
else()
    add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
endif()
