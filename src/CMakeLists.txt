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

# Every production implementation has one CMake owner.  This helper is the
# configure-time duplicate-owner gate for the final layered libraries.
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
    platform/ZeroWidthEncoder.cpp
)

set(AIAPI_APPLICATION_SOURCES
    application/account/accountManager.cpp
    application/account/AccountWorkflowSupport.cpp
    application/account/AccountSelectionPolicy.cpp
    application/account/AccountRegistrationStateMachine.cpp
    application/account/AccountSelector.cpp
    application/account/AccountTokenWorkflow.cpp
    application/account/AccountRegistrationWorkflow.cpp
    application/account/AccountHealthWorkflow.cpp
    application/account/AccountWorkers.cpp
    application/account/RetoolProvisionHealth.cpp
    application/account/AccountAdminUseCase.cpp
    application/channel/ChannelAdminUseCase.cpp
    application/health/HealthUseCase.cpp
    application/metrics/MetricsUseCase.cpp
    application/workspace/RetoolWorkspaceAdminUseCase.cpp
    application/workspace/RetoolWorkspaceUseCase.cpp
    application/channel/channelManager.cpp
    application/workspace/RetoolWorkspaceManager.cpp
    application/generation/core/Session.cpp
    application/generation/core/SessionCodec.cpp
    application/generation/core/ClientOutputSanitizer.cpp
    application/generation/core/AiApiUseCase.cpp
    application/generation/core/RetiredProviderTelemetry.cpp
    application/generation/core/RequestAdapters.cpp
    application/generation/core/GenerationService.cpp
    application/generation/core/GenerationPipeline.cpp
    application/generation/core/GenerationResponsePipeline.cpp
    application/generation/tooling/BridgeHelpers.cpp
    application/generation/tooling/BridgeProtocolCodec.cpp
    application/generation/tooling/StrictClientRules.cpp
    application/generation/tooling/ToolCallBridge.cpp
    application/generation/tooling/ForcedToolCallGenerator.cpp
    application/generation/tooling/ToolCallNormalizer.cpp
    application/generation/tooling/ToolDefinitionEncoder.cpp
    application/generation/tooling/ToolCallValidator.cpp
    application/generation/tooling/XmlTagToolCallCodec.cpp
    application/generation/continuity/ResponseIndex.cpp
    application/generation/continuity/TextExtractor.cpp
    application/generation/continuity/ContinuityResolver.cpp
    application/generation/actionProtocol/ActionProtocolCompiler.cpp
    application/generation/actionProtocol/ActionProtocolAdapter.cpp
)

set(AIAPI_INFRASTRUCTURE_SOURCES
    infrastructure/account/DrogonAccountHttpTransport.cpp
    infrastructure/account/AccountClock.cpp
    infrastructure/account/RetoolProvisionClock.cpp
    infrastructure/provider/chayns/ChaynsHttpTransport.cpp
    infrastructure/provider/chayns/ChaynsClock.cpp
    infrastructure/provider/chayns/ChaynsModelCatalog.cpp
    infrastructure/provider/chayns/ChaynsMessageCorrelation.cpp
    infrastructure/provider/chayns/chaynsapi.cpp
    infrastructure/provider/chayns/chaynsThreadReaper.cpp
    infrastructure/provider/retool/RetoolHttpTransport.cpp
    infrastructure/provider/retool/RetoolClock.cpp
    infrastructure/provider/retool/retoolapi.cpp
    infrastructure/managedAccount/backends/ClassicProviderAccountBackend.cpp
    infrastructure/managedAccount/backends/RetoolWorkspaceBackend.cpp
    infrastructure/managedAccount/service/ManagedAccountService.cpp
    infrastructure/persistence/account/accountDbManager.cpp
    infrastructure/persistence/account/accountBackupDbManager.cpp
    infrastructure/persistence/channel/channelDbManager.cpp
    infrastructure/persistence/config/ConfigDbManager.cpp
    infrastructure/persistence/chaynsThread/chaynsThreadDbManager.cpp
    infrastructure/persistence/session/SessionDbManager.cpp
    infrastructure/persistence/metrics/ErrorStatsDbManager.cpp
    infrastructure/persistence/metrics/StatusDbManager.cpp
    infrastructure/persistence/retoolWorkspace/RetoolWorkspaceDbManager.cpp
    infrastructure/metrics/ErrorStatsConfig.cpp
    infrastructure/metrics/ErrorStatsService.cpp
    infrastructure/provider/ProviderBase.cpp
    infrastructure/provider/ProviderRegistry.cpp
    infrastructure/workspace/RetoolWorkspaceService.cpp
    infrastructure/provider/limits/HistoryReplayBudget.cpp
    infrastructure/provider/limits/OutboundBudget.cpp
    infrastructure/config/ConfigValidator.cpp
)

set(AIAPI_TRANSPORT_SOURCES
    transport/controllers/sinks/ChatSseSink.cpp
    transport/controllers/sinks/ChatJsonSink.cpp
    transport/controllers/sinks/ResponsesSseSink.cpp
    transport/controllers/sinks/ResponsesJsonSink.cpp
    transport/controllers/AiApiController.cc
    transport/controllers/AccountController.cc
    transport/controllers/ChannelController.cc
    transport/controllers/MetricsController.cc
    transport/controllers/HealthController.cc
    transport/controllers/RetoolWorkspaceController.cc
    transport/controllers/RetiredProviderTombstone.cc
    transport/controllers/LogController.cc
)

set(AIAPI_RUNTIME_SOURCES
    runtime/AppContext.cpp
    runtime/AppWiring.cpp
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

# Domain remains header-only.  Application owns the use cases plus the
# generation/session/tooling closure; its external-edge dependencies are
# removed as each adapter is narrowed during P8.
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

add_executable(${PROJECT_NAME} main.cc)

# The root CMake file intentionally delegates to this directory with
# add_subdirectory(src).  Without an explicit runtime output directory CMake
# mirrors that source-directory boundary and emits the server as
# build/src/aiapi, while the operational entry point has always been
# build/aiapi.  Keep one canonical deployable path so an obsolete executable
# cannot survive a layout-only rebuild and be started by accident.
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The generic property above covers single-config generators (the production
# Makefiles/Ninja path).  Pin the standard multi-config variants too, rather
# than silently reintroducing build/<config>/aiapi for IDE generators.
foreach(aiapi_build_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_property(TARGET ${PROJECT_NAME}
                 PROPERTY RUNTIME_OUTPUT_DIRECTORY_${aiapi_build_config}
                 "${CMAKE_BINARY_DIR}")
endforeach()

# ##############################################################################
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
find_path(JSONCPP_INCLUDE_DIRS
    NAMES json/json.h
    PATHS /usr/include/jsoncpp /usr/local/include/jsoncpp
    REQUIRED)
find_library(JSONCPP_LIBRARIES
    NAMES jsoncpp
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
    REQUIRED)
get_target_property(DROGON_INCLUDE_DIRS Drogon::Drogon INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "Drogon头文件路径: ${DROGON_INCLUDE_DIRS}")

# Application still serializes its legacy request/event value boundary with
# JsonCpp, but it must not receive Drogon's HTTP/runtime usage requirements.
# Keep the codec dependency explicit until that representation is retired.
target_include_directories(aiapi_application PRIVATE ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(aiapi_application PRIVATE ${JSONCPP_LIBRARIES})

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

# Drogon HttpController registration objects have no ordinary symbol edge from
# main, so retain the formal transport archive.  All remaining production
# implementations are reachable through the formal target DAG.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-Wl,--whole-archive" aiapi_transport "-Wl,--no-whole-archive"
        aiapi_runtime)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE aiapi_transport aiapi_runtime)
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
