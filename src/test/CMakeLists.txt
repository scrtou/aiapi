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
    test_chayns_model_catalog.cpp
    test_chayns_message_correlation.cpp
    test_nexos_user_agent.cpp
    test_chayns_browser_impersonation.cpp
    test_nexos_registration_mail_policy.cpp
    test_login_response_log_summary.cpp
    test_background_task_queue_shutdown.cpp
    test_retool_workspace_store_port.cpp
    test_channel_store_port.cpp
    stub_account_manager.cpp
)

# 需要链接的项目源文件（用于测试）
set(PROJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../metrics/ErrorStatsConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/ConfigValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../metrics/ErrorStatsService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dbManager/metrics/ErrorStatsDbManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dbManager/session/SessionDbManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/continuity/ResponseIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/continuity/HistoryReplayBudget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/continuity/OutboundBudget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/continuity/TextExtractor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/continuity/ContinuityResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/core/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/core/SessionCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/core/RequestAdapters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/actionProtocol/ActionProtocolCompiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/actionProtocol/ActionProtocolAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/ToolCallBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/XmlTagToolCallCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/ToolCallValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/StrictClientRules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/ForcedToolCallGenerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/ToolCallNormalizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/BridgeHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager/tooling/BridgeProtocolCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/sinks/ChatJsonSink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/sinks/ChatSseSink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/sinks/ResponsesJsonSink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/sinks/ResponsesSseSink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../domain
    ${CMAKE_CURRENT_SOURCE_DIR}/../domain/model
    ${CMAKE_CURRENT_SOURCE_DIR}/../apiManager/ApiFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apiManager/ApiManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apipoint/chaynsapi/ChaynsModelCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apipoint/chaynsapi/ChaynsMessageCorrelation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ZeroWidthEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../retoolWorkspace/RetoolWorkspaceManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../channelManager/channelManager.cpp
)

add_executable(${PROJECT_NAME} ${TEST_SOURCES} ${PROJECT_SOURCES})

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../apiManager
    ${CMAKE_CURRENT_SOURCE_DIR}/../apipoint
    ${CMAKE_CURRENT_SOURCE_DIR}/../apipoint/chaynsapi
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionManager
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers
    ${CMAKE_CURRENT_SOURCE_DIR}/../controllers/sinks
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools
)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if (COMMAND ParseAndAddDrogonTests)
    ParseAndAddDrogonTests(${PROJECT_NAME})
else()
    add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
endif()
