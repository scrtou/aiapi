cmake_minimum_required(VERSION 3.5)
project(aiapi_test CXX)

# 测试源文件
set(TEST_SOURCES
    test_main.cc
    test_error_event.cpp
    test_error_stats_config.cpp
)

# 需要链接的项目源文件（用于测试）
set(PROJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../metrics/ErrorStatsConfig.cpp
)

add_executable(${PROJECT_NAME} ${TEST_SOURCES} ${PROJECT_SOURCES})

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

ParseAndAddDrogonTests(${PROJECT_NAME})
