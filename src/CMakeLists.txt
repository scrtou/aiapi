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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# P1-W1: runtime line/branch coverage is opt-in and isolated from release builds.
option(AIAPI_ENABLE_COVERAGE "enable gcov/llvm-cov instrumentation" OFF)

function(aiapi_enable_coverage target_name)
    if (NOT AIAPI_ENABLE_COVERAGE)
        return()
    endif()

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        # LINK_FLAGS works with the repository's CMake 3.5 minimum.
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

# P3-W1: every production implementation has one CMake owner.  This helper is
# the configure-time duplicate-owner gate used while aiapi_legacy is carved into
# the final layered libraries.
function(aiapi_register_production_sources owner)
    foreach(source_file ${ARGN})
        get_filename_component(source_abs "${source_file}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if (NOT EXISTS "${source_abs}")
            message(FATAL_ERROR "production source does not exist: ${source_file}")
        endif()
        string(MD5 source_key "${source_abs}")
        get_property(previous_owner GLOBAL PROPERTY "AIAPI_SOURCE_OWNER_${source_key}")
        if (previous_owner)
            message(FATAL_ERROR
                "production source has multiple owners: ${source_file} (${previous_owner}, ${owner})")
        endif()
        set_property(GLOBAL PROPERTY "AIAPI_SOURCE_OWNER_${source_key}" "${owner}")
    endforeach()
    target_sources(${owner} PRIVATE ${ARGN})
endfunction()

set(AIAPI_PLATFORM_SOURCES
    tools/ZeroWidthEncoder.cpp
)

set(AIAPI_APPLICATION_SOURCES
    accountManager/RetoolProvisionHealth.cpp
)

set(AIAPI_INFRASTRUCTURE_SOURCES
    accountManager/AccountHttpTransport.cpp
    accountManager/AccountClock.cpp
    accountManager/RetoolProvisionClock.cpp
    apipoint/chaynsapi/ChaynsHttpTransport.cpp
    apipoint/chaynsapi/ChaynsClock.cpp
    apipoint/chaynsapi/ChaynsModelCatalog.cpp
    apipoint/chaynsapi/ChaynsMessageCorrelation.cpp
    apipoint/retoolapi/RetoolHttpTransport.cpp
    apipoint/retoolapi/RetoolClock.cpp
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
    utils/ConfigValidator.cpp
)

set(AIAPI_TRANSPORT_SOURCES
    controllers/sinks/ChatSseSink.cpp
    controllers/sinks/ChatJsonSink.cpp
    controllers/sinks/ResponsesSseSink.cpp
    controllers/sinks/ResponsesJsonSink.cpp
    controllers/LogController.cc
)

set(AIAPI_RUNTIME_SOURCES
    utils/ApplicationShutdown.cpp
)

set(AIAPI_LEGACY_SOURCES
    accountManager/accountManager.cpp
    apiManager/ApiFactory.cpp
    apiManager/ApiManager.cpp
    apipoint/chaynsapi/chaynsapi.cpp
    apipoint/chaynsapi/chaynsThreadReaper.cpp
    apipoint/retoolapi/retoolapi.cpp
    channelManager/channelManager.cpp
    managedAccount/backends/ClassicProviderAccountBackend.cpp
    managedAccount/backends/RetoolWorkspaceBackend.cpp
    managedAccount/service/ManagedAccountService.cpp
    controllers/AiApiController.cc
    controllers/AccountController.cc
    controllers/ChannelController.cc
    controllers/MetricsController.cc
    controllers/HealthController.cc
    controllers/RetoolWorkspaceController.cc
    controllers/RetiredProviderTombstone.cc
    retoolWorkspace/RetoolWorkspaceManager.cpp
    retoolWorkspace/RetoolWorkspaceService.cpp
    sessionManager/core/Session.cpp
    sessionManager/core/SessionCodec.cpp
    sessionManager/core/ClientOutputSanitizer.cpp
    sessionManager/core/GenerationService.cpp
    sessionManager/core/GenerationServiceEmitAndToolBridge.cpp
    sessionManager/core/RetiredProviderTelemetry.cpp
    sessionManager/tooling/BridgeHelpers.cpp
    sessionManager/tooling/BridgeProtocolCodec.cpp
    sessionManager/tooling/StrictClientRules.cpp
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
)

# First P3-W3 closed module: a standard-library-only platform utility.  Keep
# the canonical src/ include root on the lowest target so every higher target
# receives exactly the same usage requirement through the target DAG.
add_library(aiapi_platform STATIC)
aiapi_register_production_sources(aiapi_platform ${AIAPI_PLATFORM_SOURCES})
target_include_directories(aiapi_platform
                           PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aiapi_platform PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_platform)

# Domain and application currently expose header contracts only.  Their JSON
# migration debt remains P3-W4; declaring the targets here establishes the
# allowed target DAG without pretending the debt has already been removed.
add_library(aiapi_domain INTERFACE)
target_link_libraries(aiapi_domain INTERFACE aiapi_platform)
target_compile_features(aiapi_domain INTERFACE cxx_std_17)

add_library(aiapi_application STATIC)
aiapi_register_production_sources(aiapi_application ${AIAPI_APPLICATION_SOURCES})
target_link_libraries(aiapi_application PUBLIC aiapi_domain aiapi_platform)
target_compile_features(aiapi_application PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_application)

add_library(aiapi_infrastructure STATIC)
aiapi_register_production_sources(aiapi_infrastructure ${AIAPI_INFRASTRUCTURE_SOURCES})
target_compile_features(aiapi_infrastructure PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_infrastructure)

add_library(aiapi_transport STATIC)
aiapi_register_production_sources(aiapi_transport ${AIAPI_TRANSPORT_SOURCES})
target_compile_features(aiapi_transport PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_transport)

add_library(aiapi_runtime STATIC)
aiapi_register_production_sources(aiapi_runtime ${AIAPI_RUNTIME_SOURCES})
target_compile_features(aiapi_runtime PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_runtime)

# Temporary P3 scaffold: main and tests link the same compiled production objects.
# Subsequent P3 work items carve closed modules out, then delete aiapi_legacy.
add_library(aiapi_legacy STATIC)
aiapi_register_production_sources(aiapi_legacy ${AIAPI_LEGACY_SOURCES})
add_executable(${PROJECT_NAME} main.cc)

# ##############################################################################
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
get_target_property(DROGON_INCLUDE_DIRS Drogon::Drogon INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "Drogon头文件路径: ${DROGON_INCLUDE_DIRS}")

target_link_libraries(aiapi_legacy PUBLIC Drogon::Drogon)
target_link_libraries(aiapi_legacy PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(aiapi_legacy PUBLIC PostgreSQL::PostgreSQL)
target_link_libraries(aiapi_legacy PUBLIC
    aiapi_application
    aiapi_infrastructure
    aiapi_transport)
target_compile_features(aiapi_legacy PUBLIC cxx_std_17)
aiapi_enable_coverage(aiapi_legacy)

target_link_libraries(aiapi_infrastructure PUBLIC
    aiapi_domain
    aiapi_platform
    Drogon::Drogon
    OpenSSL::SSL
    OpenSSL::Crypto
    PostgreSQL::PostgreSQL)
target_link_libraries(aiapi_transport PUBLIC
    aiapi_application
    aiapi_domain
    aiapi_platform
    Drogon::Drogon)
target_link_libraries(aiapi_runtime PUBLIC
    aiapi_application
    aiapi_infrastructure
    aiapi_transport)

# Provider/controller static registration objects have no ordinary symbol edge
# from main.  Retain the shrinking legacy archive plus the formal transport
# archive (which now owns LogController); tests deliberately keep ordinary
# archive extraction so their DB stubs remain isolated.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-Wl,--whole-archive" aiapi_legacy aiapi_transport "-Wl,--no-whole-archive"
        aiapi_runtime)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE aiapi_legacy aiapi_runtime)
endif()
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
aiapi_enable_coverage(${PROJECT_NAME})
# ##############################################################################


drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
                    ${CMAKE_CURRENT_BINARY_DIR})

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

option(AIAPI_BUILD_TESTS "build unit test target aiapi_test" ON)
if (AIAPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
