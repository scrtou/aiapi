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

# Define all sources in one place
add_executable(${PROJECT_NAME}
    src/main.cc
    src/accountManager/accountManager.cpp
    src/apiManager/ApiFactory.cpp
    src/apiManager/ApiManager.cpp
    src/apipoint/chaynsapi/chaynsapi.cpp
    src/channelManager/channelManager.cpp
    src/controllers/AiApi.cc
    src/controllers/sinks/ChatSseSink.cpp
    src/controllers/sinks/ChatJsonSink.cpp
    src/controllers/sinks/ResponsesSseSink.cpp
    src/dbManager/account/accountDbManager.cpp
    src/dbManager/channel/channelDbManager.cpp
    src/sessionManager/Session.cpp
    src/sessionManager/ClientOutputSanitizer.cpp
    src/sessionManager/GenerationService.cpp
)

# ##############################################################################
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
get_target_property(DROGON_INCLUDE_DIRS Drogon::Drogon INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "Drogon头文件路径: ${DROGON_INCLUDE_DIRS}")

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/apipoint/chaynsapi
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/channelManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/controllers
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/sinks
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/account
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/dbManager/channel
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/models
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/sessionManager)

# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)
# ##############################################################################

# add_subdirectory(test)
