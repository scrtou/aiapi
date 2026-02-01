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
    channelManager/channelManager.cpp
    controllers/AiApi.cc
    controllers/sinks/ChatSseSink.cpp
    controllers/sinks/ChatJsonSink.cpp
    controllers/sinks/ResponsesSseSink.cpp
    controllers/sinks/ResponsesJsonSink.cpp
    dbManager/account/accountDbManager.cpp
    dbManager/channel/channelDbManager.cpp
    sessionManager/Session.cpp
    sessionManager/ClientOutputSanitizer.cpp
    sessionManager/GenerationService.cpp
    sessionManager/RequestAdapters.cpp
    sessionManager/ToolCallBridge.cpp
    sessionManager/ToolCallValidator.cpp
    sessionManager/XmlTagToolCallCodec.cpp
    tools/ZeroWidthEncoder.cpp
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
                           ${CMAKE_CURRENT_SOURCE_DIR}/apipoint/chaynsapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/channelManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers
                           ${CMAKE_CURRENT_SOURCE_DIR}/controllers/sinks
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/account
                           ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/channel
                           ${CMAKE_CURRENT_SOURCE_DIR}/models
                           ${CMAKE_CURRENT_SOURCE_DIR}/sessionManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/tools)

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

add_subdirectory(test)
