cmake_minimum_required(VERSION 3.15)
project(ClaudeUsagePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 DLL（TrafficMonitor 插件）
add_library(ClaudeUsage SHARED src/ClaudeUsagePlugin.cpp)

target_include_directories(ClaudeUsage PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(ClaudeUsage PRIVATE winhttp user32)

if (MSVC)
    # /utf-8 让源码里的中文宽字符串字面量正确编码
    target_compile_options(ClaudeUsage PRIVATE /utf-8 /W3)
    # 静态链接运行时，避免目标机器缺少 VC++ 运行库
    set_property(TARGET ClaudeUsage PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 输出文件名固定为 ClaudeUsage.dll
set_target_properties(ClaudeUsage PROPERTIES
    OUTPUT_NAME "ClaudeUsage"
    PREFIX "")
