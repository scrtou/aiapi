cmake_minimum_required(VERSION 3.5)
project(aiapi CXX)

# 仓库根目录(本文件的上一级)下的本地 Drogon 安装,两种构建入口均适用。
get_filename_component(AIAPI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if (EXISTS "${AIAPI_REPO_ROOT}/.deps/drogon-install")
    list(PREPEND CMAKE_PREFIX_PATH "${AIAPI_REPO_ROOT}/.deps/drogon-install")
endif()

# ADR-04 (RFC-001): 语言标准钉死为 C++17,不使用探测式降级。
#
# 原实现依据 <any>/<string_view>/<coroutine> 是否存在,在 20/17/14 之间自动选择,
# 导致同一份源码在不同编译器/标准库上被编译成不同标准(语义可能不同),
# 且与 test/ 目标的兜底值(曾为 20)不一致。
#
# 现状实测:项目已使用约 146 处 C++17 特性(optional/string_view/variant/
# if constexpr/结构化绑定),Drogon 依赖亦要求 cxx_std_17。故固定为 17。
#
# 禁止使用的 C++20 特性:concepts / ranges / std::expected / std::span /
# coroutine / designated initializers / std::format / 三路比较运算符。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${PROJECT_NAME}
    main.cc
    accountManager/accountManager.cpp
    accountManager/RetoolProvisionHealth.cpp
    apiManager/ApiFactory.cpp
    apiManager/ApiManager.cpp
    apipoint/chaynsapi/chaynsapi.cpp
    apipoint/chaynsapi/ChaynsModelCatalog.cpp
    apipoint/chaynsapi/ChaynsMessageCorrelation.cpp
    apipoint/chaynsapi/chaynsThreadReaper.cpp
    apipoint/nexosapi/nexosapi.cpp
    apipoint/openai/OpenAiProvider.cpp
    apipoint/retoolapi/retoolapi.cpp
    channelManager/channelManager.cpp
    managedAccount/backends/ClassicProviderAccountBackend.cpp
    managedAccount/backends/RetoolWorkspaceBackend.cpp
    managedAccount/service/ManagedAccountService.cpp
    controllers/AiApiController.cc
    controllers/AccountController.cc
    controllers/ChannelController.cc
    controllers/MetricsController.cc
    controllers/LogController.cc
    controllers/HealthController.cc
    controllers/RetoolWorkspaceController.cc
    controllers/sinks/ChatSseSink.cpp
    controllers/sinks/ChatJsonSink.cpp
    controllers/sinks/ResponsesSseSink.cpp
    controllers/sinks/ResponsesJsonSink.cpp
    dbManager/account/accountDbManager.cpp
    dbManager/account/accountBackupDbManager.cpp
    dbManager/channel/channelDbManager.cpp
    dbManager/config/ConfigDbManager.cpp
    dbManager/chaynsThread/chaynsThreadDbManager.cpp
    dbManager/session/SessionDbManager.cpp
    dbManager/metrics/ErrorStatsDbManager.cpp
    dbManager/metrics/StatusDbManager.cpp
    dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp
    metrics/ErrorStatsConfig.cpp
    metrics/ErrorStatsService.cpp
    retoolWorkspace/RetoolWorkspaceManager.cpp
    retoolWorkspace/RetoolWorkspaceService.cpp
    sessionManager/core/Session.cpp
    sessionManager/core/SessionCodec.cpp
    sessionManager/core/ClientOutputSanitizer.cpp
    sessionManager/core/GenerationService.cpp
    sessionManager/core/GenerationServiceEmitAndToolBridge.cpp
    sessionManager/tooling/BridgeHelpers.cpp
    sessionManager/tooling/BridgeProtocolCodec.cpp
    sessionManager/tooling/StrictClientRules.cpp
    sessionManager/tooling/ToolDefinitionEncoder.cpp
    sessionManager/tooling/ForcedToolCallGenerator.cpp
    sessionManager/tooling/ToolCallNormalizer.cpp
    sessionManager/continuity/ResponseIndex.cpp
    sessionManager/continuity/HistoryReplayBudget.cpp
    sessionManager/continuity/OutboundBudget.cpp
    sessionManager/continuity/TextExtractor.cpp
    sessionManager/continuity/ContinuityResolver.cpp
    sessionManager/core/RequestAdapters.cpp
    sessionManager/actionProtocol/ActionProtocolCompiler.cpp
    sessionManager/actionProtocol/ActionProtocolAdapter.cpp
    sessionManager/tooling/ToolCallBridge.cpp
    sessionManager/tooling/ToolCallValidator.cpp
    sessionManager/tooling/XmlTagToolCallCodec.cpp
    tools/ZeroWidthEncoder.cpp
    utils/ConfigValidator.cpp
)

# ##############################################################################
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
get_target_property(DROGON_INCLUDE_DIRS Drogon::Drogon INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "Drogon头文件路径: ${DROGON_INCLUDE_DIRS}")

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${PROJECT_NAME} PRIVATE PostgreSQL::PostgreSQL)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
# ##############################################################################


drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
                    ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(${PROJECT_NAME}
                           PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/accountManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/apiManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint/openai
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint/chaynsapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint/nexosapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint/retoolapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/channelManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/managedAccount
                           ${CMAKE_CURRENT_SOURCE_DIR}/managedAccount/backends
                           ${CMAKE_CURRENT_SOURCE_DIR}/managedAccount/contracts
                           ${CMAKE_CURRENT_SOURCE_DIR}/managedAccount/service
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers/sinks
                           ${CMAKE_CURRENT_SOURCE_DIR}/domain
                           ${CMAKE_CURRENT_SOURCE_DIR}/domain/model
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/account
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/channel
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/chaynsThread
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/config
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/retoolWorkspace
                           ${CMAKE_CURRENT_SOURCE_DIR}/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/models
                           ${CMAKE_CURRENT_SOURCE_DIR}/retoolWorkspace
                           ${CMAKE_CURRENT_SOURCE_DIR}/sessionManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/tools
                           ${CMAKE_CURRENT_SOURCE_DIR}/utils)

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

option(AIAPI_BUILD_TESTS "build unit test target aiapi_test" ON)
if (AIAPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
