cmake_minimum_required(VERSION 3.5)
project(aiapi CXX)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/drogon-install")
    list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/drogon-install")
endif()

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
    src/main.cc
    src/accountManager/accountManager.cpp
    src/apiManager/ApiFactory.cpp
    src/apiManager/ApiManager.cpp
    src/apipoint/chaynsapi/chaynsapi.cpp
    src/apipoint/nexosapi/nexosapi.cpp
    src/apipoint/openai/OpenAiProvider.cpp
    src/apipoint/retoolapi/retoolapi.cpp
    src/channelManager/channelManager.cpp
    src/managedAccount/backends/ClassicProviderAccountBackend.cpp
    src/managedAccount/backends/RetoolWorkspaceBackend.cpp
    src/managedAccount/service/ManagedAccountService.cpp
    src/controllers/AiApiController.cc
    src/controllers/AccountController.cc
    src/controllers/ChannelController.cc
    src/controllers/MetricsController.cc
    src/controllers/LogController.cc
    src/controllers/HealthController.cc
    src/controllers/RetoolWorkspaceController.cc
    src/controllers/sinks/ChatSseSink.cpp
    src/controllers/sinks/ChatJsonSink.cpp
    src/controllers/sinks/ResponsesSseSink.cpp
    src/controllers/sinks/ResponsesJsonSink.cpp
    src/dbManager/account/accountDbManager.cpp
    src/dbManager/account/accountBackupDbManager.cpp
    src/dbManager/channel/channelDbManager.cpp
    src/dbManager/config/ConfigDbManager.cpp
    src/dbManager/metrics/ErrorStatsDbManager.cpp
    src/dbManager/metrics/StatusDbManager.cpp
    src/dbManager/retoolWorkspace/RetoolWorkspaceDbManager.cpp
    src/metrics/ErrorStatsConfig.cpp
    src/metrics/ErrorStatsService.cpp
    src/retoolWorkspace/RetoolWorkspaceManager.cpp
    src/retoolWorkspace/RetoolWorkspaceService.cpp
    src/sessionManager/core/Session.cpp
    src/sessionManager/core/ClientOutputSanitizer.cpp
    src/sessionManager/core/GenerationService.cpp
    src/sessionManager/core/GenerationServiceEmitAndToolBridge.cpp
    src/sessionManager/tooling/BridgeHelpers.cpp
    src/sessionManager/tooling/StrictClientRules.cpp
    src/sessionManager/tooling/ToolDefinitionEncoder.cpp
    src/sessionManager/tooling/ForcedToolCallGenerator.cpp
    src/sessionManager/tooling/ToolCallNormalizer.cpp
    src/sessionManager/continuity/ResponseIndex.cpp
    src/sessionManager/continuity/TextExtractor.cpp
    src/sessionManager/continuity/ContinuityResolver.cpp
    src/sessionManager/core/RequestAdapters.cpp
    src/sessionManager/tooling/ToolCallBridge.cpp
    src/sessionManager/tooling/ToolCallValidator.cpp
    src/sessionManager/tooling/XmlTagToolCallCodec.cpp
    src/tools/ZeroWidthEncoder.cpp
    src/utils/ConfigValidator.cpp
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

drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/views
                    ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(${PROJECT_NAME}
                           PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/accountManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apiManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint/openai
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint/chaynsapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint/nexosapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint/retoolapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/channelManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/managedAccount
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/managedAccount/backends
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/managedAccount/contracts
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/managedAccount/service
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/controllers
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/sinks
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/account
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/channel
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/config
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/retoolWorkspace
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/models
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/retoolWorkspace
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/sessionManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/tools
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/utils)

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

# add_subdirectory(test)
