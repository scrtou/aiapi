cmake_minimum_required(VERSION 3.5)
project(aiapi CXX)

include(CheckIncludeFileCXX)

check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)
if (NOT "${CMAKE_CXX_STANDARD}" STREQUAL "")
    # Do nothing
elseif (HAS_ANY AND HAS_STRING_VIEW AND HAS_COROUTINE)
    set(CMAKE_CXX_STANDARD 20)
elseif (HAS_ANY AND HAS_STRING_VIEW)
    set(CMAKE_CXX_STANDARD 17)
else ()
    set(CMAKE_CXX_STANDARD 14)
endif ()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${PROJECT_NAME}
    main.cc
    accountManager/accountManager.cpp
    apiManager/ApiFactory.cpp
    apiManager/ApiManager.cpp
    apipoint/chaynsapi/chaynsapi.cpp
    apipoint/openai/OpenAiProvider.cpp
    channelManager/channelManager.cpp
    controllers/AiApiController.cc
    controllers/AccountController.cc
    controllers/ChannelController.cc
    controllers/MetricsController.cc
    controllers/LogController.cc
    controllers/HealthController.cc
    controllers/sinks/ChatSseSink.cpp
    controllers/sinks/ChatJsonSink.cpp
    controllers/sinks/ResponsesSseSink.cpp
    controllers/sinks/ResponsesJsonSink.cpp
    dbManager/account/accountDbManager.cpp
    dbManager/channel/channelDbManager.cpp
    dbManager/metrics/ErrorStatsDbManager.cpp
    dbManager/metrics/StatusDbManager.cpp
    metrics/ErrorStatsConfig.cpp
    metrics/ErrorStatsService.cpp
    sessionManager/core/Session.cpp
    sessionManager/core/ClientOutputSanitizer.cpp
    sessionManager/core/GenerationService.cpp
    sessionManager/core/GenerationServiceEmitAndToolBridge.cpp
    sessionManager/tooling/BridgeHelpers.cpp
    sessionManager/tooling/StrictClientRules.cpp
    sessionManager/tooling/ToolDefinitionEncoder.cpp
    sessionManager/tooling/ForcedToolCallGenerator.cpp
    sessionManager/tooling/ToolCallNormalizer.cpp
    sessionManager/continuity/ResponseIndex.cpp
    sessionManager/continuity/TextExtractor.cpp
    sessionManager/continuity/ContinuityResolver.cpp
    sessionManager/core/RequestAdapters.cpp
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
# ##############################################################################

if (CMAKE_CXX_STANDARD LESS 17)
    message(FATAL_ERROR "c++17 or higher is required")
elseif (CMAKE_CXX_STANDARD LESS 20)
    message(STATUS "use c++17")
else ()
    message(STATUS "use c++20")
endif ()

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
                           ${CMAKE_CURRENT_SOURCE_DIR}/channelManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers/sinks
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/account
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/channel
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/models
                           ${CMAKE_CURRENT_SOURCE_DIR}/sessionManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/tools
                           ${CMAKE_CURRENT_SOURCE_DIR}/utils)

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

add_subdirectory(test)
